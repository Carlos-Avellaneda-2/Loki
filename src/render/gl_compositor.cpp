#include "render/gl_compositor.h"

#include "render/color_conversion.h"

#include <QMatrix3x3>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace loki::render {

namespace {

// Layer pass: the unit quad is mapped by `toNdc` (layer pixels -> canvas -> NDC); the fragment
// converts YUV to RGB, blends with the accumulated image and applies opacity.
constexpr auto kLayerVertex = R"(#version 330 core
layout(location = 0) in vec2 position;
layout(location = 1) in vec2 texCoord;
uniform mat3 toNdc;
out vec2 uv;
void main() {
    uv = texCoord;
    vec3 p = toNdc * vec3(position, 1.0);
    gl_Position = vec4(p.xy, 0.0, 1.0);
}
)";

constexpr auto kLayerFragment = R"(#version 330 core
in vec2 uv;
out vec4 color;
// 0 = YUV420P, 1 = NV12, 2 = RGBA, 3 = nested level texture, 4 = adjustment (the image so far),
// 5 = prepared source (prepass: key applied in plane0, blurred copy in plane1; rows bottom-up).
uniform int format;
uniform sampler2D plane0;
uniform sampler2D plane1;
uniform sampler2D plane2;
uniform sampler2D accum;
uniform vec2 targetSize;
uniform mat3 yuvMatrix;
uniform vec3 yuvOffset;
uniform float opacity;
uniform int blendMode;   // model::BlendMode
// Grading (render::LayerGrade), same order and formulas as render::gradePixel.
uniform bool colorOn;
uniform float exposure;
uniform float contrast;
uniform float saturation;
uniform float temperature;
uniform bool lutOn;
uniform sampler3D lut;
uniform float lutSize;
uniform vec3 lutMin;
uniform vec3 lutMax;
uniform float lutMix;
uniform float dim;
uniform float dimTarget;
// Lumetri (render::lumetriPixel).
uniform bool lumOn;
uniform vec3 wheelOffset[3];
uniform vec3 wheelLuma;
uniform bool curvesOn;
uniform sampler2D curves; // 256 x 1, RGB
uniform bool hslOn;
uniform float hslHueCenter;
uniform float hslHueRange;
uniform float hslSoftness;
uniform float hslSatMin;
uniform float hslLumaMin;
uniform float hslLumaMax;
uniform float hslHueShift;
uniform float hslSaturation;
uniform float hslLuma;
uniform bool hslShowMask;
uniform float vigAmount;
uniform float vigMidpoint;
uniform float vigFeather;
// Ultra Key (render::keyPixel), applied here unless the source was prepared.
uniform bool keyOn;
uniform vec3 keyColor;
uniform float keyTolerance;
uniform float keySoftness;
uniform float keyChoke;
uniform float keySpill;
uniform bool keyMatte;
// Prepared source mix (format 5).
uniform bool blurOn;
uniform float sharpen;
// Per-stage masks (render::LayerMasks), sampled at the layer coordinate.
uniform bool maskOpacityOn;
uniform bool maskColorOn;
uniform bool maskLumetriOn;
uniform bool maskLutOn;
uniform bool maskBlurOn;
uniform sampler2D maskOpacity;
uniform sampler2D maskColor;
uniform sampler2D maskLumetri;
uniform sampler2D maskLut;
uniform sampler2D maskBlur;
// Wipe / iris mask (render::wipeAlpha).
uniform bool wipeOn;
uniform float wipeProgress;
uniform float wipeSoftness;
uniform bool wipeReverse;
uniform int wipeShape;
uniform float wipeAspect;

float luma709(vec3 c) {
    return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

float maskAt(bool on, sampler2D m) {
    return on ? texture(m, uv).r : 1.0;
}

vec4 keyed(vec4 s) {
    if (!keyOn) return s;
    float y = luma709(s.rgb);
    vec2 c = vec2((s.b - y) / 1.8556, (s.r - y) / 1.5748);
    float ky = luma709(keyColor);
    vec2 k = vec2((keyColor.b - ky) / 1.8556, (keyColor.r - ky) / 1.5748);
    float dist = length(c - k) * 2.0;
    float a = smoothstep(keyTolerance, keyTolerance + max(keySoftness, 0.001), dist);
    a = clamp((a - keyChoke) / max(1.0 - keyChoke, 0.001), 0.0, 1.0);
    if (keyMatte) return vec4(vec3(a), s.a);
    vec3 dir = keyColor - ky;
    float len = length(dir);
    vec3 rgb = s.rgb;
    if (len > 1e-4 && keySpill > 0.0) {
        float amount = max(0.0, dot(rgb - y, dir) / len);
        rgb = clamp(rgb - dir / len * amount * keySpill, 0.0, 1.0);
    }
    return vec4(rgb, s.a * a);
}

vec4 sourceColor() {
    if (format == 2) {
        return keyed(texture(plane0, uv));
    }
    if (format == 3) {
        return texture(plane0, vec2(uv.x, 1.0 - uv.y));
    }
    if (format == 4) {
        return texture(accum, gl_FragCoord.xy / targetSize);
    }
    if (format == 5) {
        vec2 t = vec2(uv.x, 1.0 - uv.y);
        vec4 a = texture(plane0, t);
        vec4 b = texture(plane1, t);
        float m = maskAt(maskBlurOn, maskBlur);
        float mb = blurOn ? m : 0.0;
        vec3 rgb = clamp(a.rgb + (b.rgb - a.rgb) * mb + (a.rgb - b.rgb) * sharpen * m, 0.0, 1.0);
        return vec4(rgb, a.a + (b.a - a.a) * mb);
    }
    float y = texture(plane0, uv).r;
    vec2 c = format == 1 ? texture(plane1, uv).rg : vec2(texture(plane1, uv).r, texture(plane2, uv).r);
    return keyed(vec4(clamp(yuvMatrix * vec3(y, c.x, c.y) + yuvOffset, 0.0, 1.0), 1.0));
}

float overlayChannel(float s, float d) {
    return d < 0.5 ? 2.0 * s * d : 1.0 - 2.0 * (1.0 - s) * (1.0 - d);
}

vec3 blend(vec3 s, vec3 d) {
    if (blendMode == 1) return s * d;
    if (blendMode == 2) return 1.0 - (1.0 - s) * (1.0 - d);
    if (blendMode == 3) return vec3(overlayChannel(s.r, d.r), overlayChannel(s.g, d.g), overlayChannel(s.b, d.b));
    if (blendMode == 4) return min(s + d, vec3(1.0));
    if (blendMode == 5) return min(s, d);
    if (blendMode == 6) return max(s, d);
    if (blendMode == 7) return abs(s - d);
    return s;
}

float hueToRgb(float p, float q, float t) {
    if (t < 0.0) t += 1.0;
    if (t > 1.0) t -= 1.0;
    if (t < 1.0 / 6.0) return p + (q - p) * 6.0 * t;
    if (t < 0.5) return q;
    if (t < 2.0 / 3.0) return p + (q - p) * (2.0 / 3.0 - t) * 6.0;
    return p;
}

vec3 rgbToHsl(vec3 c) {
    float mx = max(c.r, max(c.g, c.b));
    float mn = min(c.r, min(c.g, c.b));
    float l = (mx + mn) * 0.5;
    float d = mx - mn;
    if (d < 1e-6) return vec3(0.0, 0.0, l);
    float s = l > 0.5 ? d / (2.0 - mx - mn) : d / (mx + mn);
    float h;
    if (mx == c.r) h = (c.g - c.b) / d + (c.g < c.b ? 6.0 : 0.0);
    else if (mx == c.g) h = (c.b - c.r) / d + 2.0;
    else h = (c.r - c.g) / d + 4.0;
    return vec3(h * 60.0, s, l);
}

vec3 hslToRgb(vec3 hsl) {
    float h = hsl.x / 360.0;
    float s = clamp(hsl.y, 0.0, 1.0);
    float l = clamp(hsl.z, 0.0, 1.0);
    if (s <= 0.0) return vec3(l);
    float q = l < 0.5 ? l * (1.0 + s) : l + s - l * s;
    float p = 2.0 * l - q;
    return vec3(hueToRgb(p, q, h + 1.0 / 3.0), hueToRgb(p, q, h), hueToRgb(p, q, h - 1.0 / 3.0));
}

vec3 lumetri(vec3 c) {
    float l = clamp(luma709(c), 0.0, 1.0);
    float ws = (1.0 - l) * (1.0 - l);
    float wh = l * l;
    float wm = 1.0 - ws - wh;
    c = clamp(c + ws * (wheelOffset[0] + wheelLuma.x) + wm * (wheelOffset[1] + wheelLuma.y) +
              wh * (wheelOffset[2] + wheelLuma.z), 0.0, 1.0);
    if (curvesOn) {
        vec3 x = (clamp(c, 0.0, 1.0) * 255.0 + 0.5) / 256.0;
        c = vec3(texture(curves, vec2(x.r, 0.5)).r, texture(curves, vec2(x.g, 0.5)).g, texture(curves, vec2(x.b, 0.5)).b);
    }
    if (hslOn) {
        vec3 hsl = rgbToHsl(c);
        float dh = mod(abs(hsl.x - hslHueCenter), 360.0);
        dh = min(dh, 360.0 - dh);
        float soft = max(hslSoftness, 0.001);
        float qh = 1.0 - smoothstep(hslHueRange, hslHueRange + soft, dh);
        float qs = smoothstep(hslSatMin - soft, hslSatMin, hsl.y * 100.0);
        float ql = smoothstep(hslLumaMin - soft, hslLumaMin, hsl.z * 100.0) *
                   (1.0 - smoothstep(hslLumaMax, hslLumaMax + soft, hsl.z * 100.0));
        float sel = qh * qs * ql;
        if (hslShowMask) {
            c = vec3(sel);
        } else {
            float h = mod(hsl.x + hslHueShift + 360.0, 360.0);
            vec3 fixedColor = hslToRgb(vec3(h, clamp(hsl.y * hslSaturation / 100.0, 0.0, 1.0), clamp(hsl.z + hslLuma / 200.0, 0.0, 1.0)));
            c = c + (fixedColor - c) * sel;
        }
    }
    if (vigAmount != 0.0) {
        float d = length(uv - vec2(0.5)) / 0.70710678;
        float start = vigMidpoint / 100.0;
        float f = smoothstep(start, start + max(vigFeather / 100.0, 0.01), d);
        float a = clamp(vigAmount, -100.0, 100.0) / 100.0;
        c = a < 0.0 ? c * (1.0 + a * f) : c + (1.0 - c) * a * f;
    }
    return c;
}

vec3 grade(vec3 c) {
    if (colorOn) {
        vec3 a = (c * exp2(exposure) - 0.5) * (1.0 + contrast / 100.0) + 0.5;
        float luma = luma709(a);
        a = luma + (a - luma) * (saturation / 100.0);
        a.r *= 1.0 + 0.1 * temperature / 100.0;
        a.b *= 1.0 - 0.1 * temperature / 100.0;
        a = clamp(a, 0.0, 1.0);
        c = c + (a - c) * maskAt(maskColorOn, maskColor);
    }
    if (lumOn) {
        c = c + (lumetri(c) - c) * maskAt(maskLumetriOn, maskLumetri);
    }
    if (lutOn && lutMix > 0.0) {
        vec3 x = clamp((c - lutMin) / max(lutMax - lutMin, vec3(1e-6)), 0.0, 1.0);
        vec3 l = texture(lut, (x * (lutSize - 1.0) + 0.5) / lutSize).rgb;
        c = c + (l - c) * clamp(lutMix, 0.0, 1.0) * maskAt(maskLutOn, maskLut);
    }
    if (dim > 0.0) {
        c = c + (vec3(dimTarget) - c) * clamp(dim, 0.0, 1.0);
    }
    return c;
}

float wipeAlpha(vec2 p) {
    if (!wipeOn) return 1.0;
    float soft = max(wipeSoftness, 1e-4);
    float distance = p.x;
    float reach = 1.0;
    if (wipeShape == 1) {
        vec2 d = vec2((p.x - 0.5) * wipeAspect, p.y - 0.5);
        distance = length(d);
        reach = sqrt(0.25 * wipeAspect * wipeAspect + 0.25);
    } else if (wipeShape == 2) {
        distance = max(abs(p.x - 0.5), abs(p.y - 0.5)) * 2.0;
    }
    float edge = -soft + wipeProgress * (reach + 2.0 * soft);
    float t = clamp((edge - distance) / soft + 0.5, 0.0, 1.0);
    float shown = t * t * (3.0 - 2.0 * t);
    return wipeReverse ? 1.0 - shown : shown;
}

// render::compositeOver: blend where the destination is opaque, then straight-alpha "over".
void main() {
    vec4 dst = texture(accum, gl_FragCoord.xy / targetSize);
    vec4 s = sourceColor();
    vec3 src = grade(s.rgb);
    vec2 canvas = vec2(gl_FragCoord.x / targetSize.x, 1.0 - gl_FragCoord.y / targetSize.y);
    float a = clamp(opacity, 0.0, 1.0) * maskAt(maskOpacityOn, maskOpacity) * clamp(s.a, 0.0, 1.0) * wipeAlpha(canvas);
    float da = clamp(dst.a, 0.0, 1.0);
    vec3 blended = mix(src, blend(src, dst.rgb), da);
    float outA = a + da * (1.0 - a);
    vec3 rgb = outA > 0.0 ? (blended * a + dst.rgb * da * (1.0 - a)) / outA : vec3(0.0);
    color = vec4(rgb, outA);
}
)";

// Prepass (D35/D37): decoded frame -> RGBA with the key applied, at the frame's size. Rows are
// written top of the picture at the top of the target (sampled later with a flipped v).
constexpr auto kPrepareVertex = R"(#version 330 core
layout(location = 0) in vec2 position;
layout(location = 1) in vec2 texCoord;
out vec2 uv;
void main() {
    uv = texCoord;
    gl_Position = vec4(position.x * 2.0 - 1.0, 1.0 - position.y * 2.0, 0.0, 1.0);
}
)";

constexpr auto kPrepareFragment = R"(#version 330 core
in vec2 uv;
out vec4 color;
uniform int format;
uniform sampler2D plane0;
uniform sampler2D plane1;
uniform sampler2D plane2;
uniform mat3 yuvMatrix;
uniform vec3 yuvOffset;
uniform bool keyOn;
uniform vec3 keyColor;
uniform float keyTolerance;
uniform float keySoftness;
uniform float keyChoke;
uniform float keySpill;
uniform bool keyMatte;
float luma709(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }
vec4 keyed(vec4 s) {
    if (!keyOn) return s;
    float y = luma709(s.rgb);
    vec2 c = vec2((s.b - y) / 1.8556, (s.r - y) / 1.5748);
    float ky = luma709(keyColor);
    vec2 k = vec2((keyColor.b - ky) / 1.8556, (keyColor.r - ky) / 1.5748);
    float dist = length(c - k) * 2.0;
    float a = smoothstep(keyTolerance, keyTolerance + max(keySoftness, 0.001), dist);
    a = clamp((a - keyChoke) / max(1.0 - keyChoke, 0.001), 0.0, 1.0);
    if (keyMatte) return vec4(vec3(a), s.a);
    vec3 dir = keyColor - ky;
    float len = length(dir);
    vec3 rgb = s.rgb;
    if (len > 1e-4 && keySpill > 0.0) {
        float amount = max(0.0, dot(rgb - y, dir) / len);
        rgb = clamp(rgb - dir / len * amount * keySpill, 0.0, 1.0);
    }
    return vec4(rgb, s.a * a);
}
void main() {
    if (format == 2) {
        color = keyed(texture(plane0, uv));
        return;
    }
    float y = texture(plane0, uv).r;
    vec2 c = format == 1 ? texture(plane1, uv).rg : vec2(texture(plane1, uv).r, texture(plane2, uv).r);
    color = keyed(vec4(clamp(yuvMatrix * vec3(y, c.x, c.y) + yuvOffset, 0.0, 1.0), 1.0));
}
)";

// Separable gaussian (render::blurKernel taps), exact texel offsets with clamp to edge.
constexpr auto kBlurFragment = R"(#version 330 core
out vec4 color;
uniform sampler2D source;
uniform ivec2 direction;
uniform int tapCount;
uniform int offsets[65];
uniform float weights[65];
void main() {
    ivec2 size = textureSize(source, 0);
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec4 sum = vec4(0.0);
    for (int i = 0; i < tapCount; ++i) {
        ivec2 q = clamp(p + direction * offsets[i], ivec2(0), size - 1);
        sum += texelFetch(source, q, 0) * weights[i];
    }
    color = sum;
}
)";

// Present pass: draws the accumulated texture into the window (letterboxed by the vertex rect).
constexpr auto kPresentVertex = R"(#version 330 core
layout(location = 0) in vec2 position;
layout(location = 1) in vec2 texCoord;
uniform vec4 rect;
out vec2 uv;
void main() {
    uv = texCoord; // NDC and framebuffer textures are both bottom-up
    gl_Position = vec4(rect.xy + position * rect.zw, 0.0, 1.0);
}
)";

constexpr auto kPresentFragment = R"(#version 330 core
in vec2 uv;
out vec4 color;
uniform sampler2D image;
void main() {
    vec4 c = texture(image, uv);
    color = vec4(c.rgb * clamp(c.a, 0.0, 1.0), 1.0); // over black
}
)";

// Unit quad (position 0..1, y down in layer pixels; texture coordinate matches).
// Nesting deeper than this is not drawn (cycles are forbidden by the model).
constexpr std::size_t kMaxNestingDepth = 16;

constexpr float kQuad[] = {
    0.0f, 0.0f, 0.0f, 0.0f, //
    1.0f, 0.0f, 1.0f, 0.0f, //
    0.0f, 1.0f, 0.0f, 1.0f, //
    1.0f, 1.0f, 1.0f, 1.0f, //
};

QOpenGLExtraFunctions* gl() {
    return QOpenGLContext::currentContext()->extraFunctions();
}

bool build(QOpenGLShaderProgram& program, const char* vertex, const char* fragment) {
    if (!program.addShaderFromSourceCode(QOpenGLShader::Vertex, vertex) ||
        !program.addShaderFromSourceCode(QOpenGLShader::Fragment, fragment) || !program.link()) {
        spdlog::error("Compositor shader failed: {}", program.log().toStdString());
        return false;
    }
    return true;
}

} // namespace

bool GlCompositor::initialize() {
    if (m_initialized) {
        return true;
    }
    if (!build(m_layerProgram, kLayerVertex, kLayerFragment) || !build(m_presentProgram, kPresentVertex, kPresentFragment) ||
        !build(m_prepareProgram, kPrepareVertex, kPrepareFragment) || !build(m_blurProgram, kPrepareVertex, kBlurFragment)) {
        return false;
    }
    auto* f = gl();
    m_vao.create();
    m_vao.bind();
    m_vbo.create();
    m_vbo.bind();
    m_vbo.allocate(kQuad, sizeof(kQuad));
    f->glEnableVertexAttribArray(0);
    f->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
    f->glEnableVertexAttribArray(1);
    f->glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), reinterpret_cast<void*>(2 * sizeof(float)));
    m_vao.release();
    m_vbo.release();
    m_initialized = true;
    return true;
}

GlCompositor::Level& GlCompositor::ensureLevel(std::size_t depth, int width, int height) {
    if (m_levels.size() <= depth) {
        m_levels.resize(depth + 1);
    }
    auto& level = m_levels[depth];
    if (width == level.width && height == level.height && level.targets[0].fbo != 0) {
        return level;
    }
    auto* f = gl();
    for (auto& t : level.targets) {
        if (t.fbo == 0) {
            f->glGenFramebuffers(1, &t.fbo);
            f->glGenTextures(1, &t.texture);
        }
        f->glBindTexture(GL_TEXTURE_2D, t.texture);
        f->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_HALF_FLOAT, nullptr);
        f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        f->glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
        f->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.texture, 0);
        if (f->glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            spdlog::error("Compositor framebuffer {}x{} is incomplete", width, height);
        }
    }
    f->glBindTexture(GL_TEXTURE_2D, 0);
    level.width = width;
    level.height = height;
    return level;
}

void GlCompositor::uploadLayer(std::size_t slot, const media::VideoFrame& frame) {
    auto* f = gl();
    while (m_layerTextures.size() <= slot) {
        std::array<unsigned int, 3> tex{};
        f->glGenTextures(3, tex.data());
        for (const auto t : tex) {
            f->glBindTexture(GL_TEXTURE_2D, t);
            f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        m_layerTextures.push_back(tex);
    }
    f->glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    for (int i = 0; i < frame.planeCount; ++i) {
        const auto p = static_cast<std::size_t>(i);
        GLenum internal = GL_R8;
        GLenum format = GL_RED;
        int bytesPerPixel = 1;
        if (frame.format == media::PixelFormat::Rgba) {
            internal = GL_RGBA8;
            format = GL_RGBA;
            bytesPerPixel = 4;
        } else if (frame.format == media::PixelFormat::Nv12 && i == 1) {
            internal = GL_RG8;
            format = GL_RG;
            bytesPerPixel = 2;
        }
        f->glBindTexture(GL_TEXTURE_2D, m_layerTextures[slot][p]);
        f->glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(internal), frame.strides[p] / bytesPerPixel,
                        frame.planeHeights[p], 0, format, GL_UNSIGNED_BYTE, frame.planes[p].data());
    }
    f->glBindTexture(GL_TEXTURE_2D, 0);
}

void GlCompositor::render(const CompositeFrame& frame, int width, int height) {
    if (!m_initialized || width <= 0 || height <= 0) {
        return;
    }
    auto* f = gl();
    m_canvasAspect = frame.height > 0 ? static_cast<double>(frame.width) / frame.height : 16.0 / 9.0;
    GLint previousFbo = 0;
    f->glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFbo);
    m_nextSlot = 0;
    renderLevel(frame, width, height, 0);
    m_width = width;
    m_height = height;
    f->glActiveTexture(GL_TEXTURE4);
    f->glBindTexture(GL_TEXTURE_3D, 0);
    f->glActiveTexture(GL_TEXTURE0);
    f->glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previousFbo));
    // Drop LUT textures no layer used in this frame.
    for (auto it = m_luts.begin(); it != m_luts.end();) {
        if (!it->second.usedThisFrame) {
            f->glDeleteTextures(1, &it->second.texture);
            it = m_luts.erase(it);
        } else {
            it->second.usedThisFrame = false;
            ++it;
        }
    }
}

bool GlCompositor::renderLevel(const CompositeFrame& frame, int width, int height, std::size_t depth) {
    if (depth > kMaxNestingDepth) {
        return false;
    }
    auto* f = gl();
    ensureLevel(depth, width, height);
    // Index access only: nested levels may grow m_levels (references would dangle).
    const auto targets = [&](int i) -> Target& { return m_levels[depth].targets[static_cast<std::size_t>(i)]; };
    m_levels[depth].current = 0;
    f->glBindFramebuffer(GL_FRAMEBUFFER, targets(0).fbo);
    f->glViewport(0, 0, width, height);
    f->glClearColor(0.0f, 0.0f, 0.0f, frame.transparent ? 0.0f : 1.0f);
    f->glClear(GL_COLOR_BUFFER_BIT);
    const double cw = frame.width > 0 ? frame.width : width;
    const double ch = frame.height > 0 ? frame.height : height;

    for (const auto& layer : frame.layers) {
        if (!layer.hasPicture() || layer.transform.opacity <= 0.0 || layer.sourceWidth <= 0 || layer.sourceHeight <= 0) {
            continue;
        }
        // The picture: a nested level, the image so far (adjustment) or an uploaded frame.
        unsigned int nestedTexture = 0;
        std::size_t slot = 0;
        if (layer.nested) {
            const int nw = std::max(1, static_cast<int>(std::lround(layer.sourceWidth * width / cw)));
            const int nh = std::max(1, static_cast<int>(std::lround(layer.sourceHeight * height / ch)));
            if (!renderLevel(*layer.nested, nw, nh, depth + 1)) {
                continue;
            }
            const auto& inner = m_levels[depth + 1];
            nestedTexture = inner.targets[static_cast<std::size_t>(inner.current)].texture;
        } else if (!layer.adjustment) {
            slot = m_nextSlot++;
            uploadLayer(slot, *layer.frame);
        }
        const bool prepared = !layer.nested && !layer.adjustment && (layer.key.enabled || layer.blur.active());
        if (prepared) {
            preparePass(layer, slot);
        }

        // Copy the accumulated image to the other target, then draw the layer into it.
        const int src = m_levels[depth].current;
        const int dst = 1 - src;
        f->glBindFramebuffer(GL_READ_FRAMEBUFFER, targets(src).fbo);
        f->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, targets(dst).fbo);
        f->glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        f->glBindFramebuffer(GL_FRAMEBUFFER, targets(dst).fbo);
        f->glViewport(0, 0, width, height);

        m_layerProgram.bind();
        m_vao.bind();
        m_layerProgram.setUniformValue("targetSize", QVector2D(static_cast<float>(width), static_cast<float>(height)));
        // layer pixels -> canvas pixels -> NDC (canvas y down, NDC y up).
        const auto m = layerToCanvas(layer);
        const double sx = layer.sourceWidth;
        const double sy = layer.sourceHeight;
        // Unit quad -> layer pixels: scale by (sx, sy).
        const double a = m[0] * sx;
        const double b = m[1] * sy;
        const double c = m[2];
        const double d = m[3] * sx;
        const double e = m[4] * sy;
        const double g = m[5];
        const float toNdc[9] = {static_cast<float>(2.0 * a / cw),  static_cast<float>(2.0 * b / cw),
                                static_cast<float>(2.0 * c / cw - 1.0), static_cast<float>(-2.0 * d / ch),
                                static_cast<float>(-2.0 * e / ch), static_cast<float>(1.0 - 2.0 * g / ch),
                                0.0f, 0.0f, 1.0f};
        m_layerProgram.setUniformValue("toNdc", QMatrix3x3(toNdc));
        int format = 4;
        if (layer.nested) {
            format = 3;
        } else if (prepared) {
            format = 5;
        } else if (!layer.adjustment) {
            const auto conv = yuvToRgb(layer.frame->matrix, layer.frame->range);
            m_layerProgram.setUniformValue("yuvMatrix", QMatrix3x3(conv.matrix.data()));
            m_layerProgram.setUniformValue("yuvOffset", QVector3D(conv.offset[0], conv.offset[1], conv.offset[2]));
            format = layer.frame->format == media::PixelFormat::Yuv420P ? 0
                     : layer.frame->format == media::PixelFormat::Nv12  ? 1
                                                                        : 2;
        }
        m_layerProgram.setUniformValue("format", format);
        m_layerProgram.setUniformValue("opacity", static_cast<float>(layer.transform.opacity / 100.0));
        m_layerProgram.setUniformValue("blendMode", static_cast<int>(layer.transform.blend));
        const auto& lg = layer.grade;
        m_layerProgram.setUniformValue("colorOn", !lg.color.isIdentity());
        m_layerProgram.setUniformValue("exposure", lg.color.exposure);
        m_layerProgram.setUniformValue("contrast", lg.color.contrast);
        m_layerProgram.setUniformValue("saturation", lg.color.saturation);
        m_layerProgram.setUniformValue("temperature", lg.color.temperature);
        m_layerProgram.setUniformValue("dim", lg.dim);
        const unsigned int lutTexture = lg.lut ? lutTextureFor(lg.lut) : 0;
        m_layerProgram.setUniformValue("lutOn", lutTexture != 0);
        f->glActiveTexture(GL_TEXTURE4);
        f->glBindTexture(GL_TEXTURE_3D, lutTexture);
        m_layerProgram.setUniformValue("lut", 4);
        if (lg.lut) {
            m_layerProgram.setUniformValue("lutSize", static_cast<float>(lg.lut->size));
            m_layerProgram.setUniformValue("lutMin", QVector3D(lg.lut->domainMin[0], lg.lut->domainMin[1], lg.lut->domainMin[2]));
            m_layerProgram.setUniformValue("lutMax", QVector3D(lg.lut->domainMax[0], lg.lut->domainMax[1], lg.lut->domainMax[2]));
        }
        m_layerProgram.setUniformValue("lutMix", lg.lutMix);
        m_layerProgram.setUniformValue("wipeOn", layer.wipe.enabled);
        m_layerProgram.setUniformValue("wipeProgress", layer.wipe.progress);
        m_layerProgram.setUniformValue("wipeSoftness", layer.wipe.softness);
        m_layerProgram.setUniformValue("wipeReverse", layer.wipe.reverse);
        m_layerProgram.setUniformValue("wipeShape", static_cast<int>(layer.wipe.shape));
        m_layerProgram.setUniformValue("wipeAspect", layer.wipe.aspect);
        m_layerProgram.setUniformValue("dimTarget", lg.dimTarget);
        setPhase14Uniforms(layer, prepared);
        for (int p = 0; p < 3; ++p) {
            f->glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(p));
            unsigned int texture = 0;
            if (layer.nested) {
                texture = p == 0 ? nestedTexture : 0;
            } else if (prepared) {
                texture = p == 0 ? m_prep[0].texture : p == 1 ? m_prep[2].texture : 0;
            } else if (!layer.adjustment) {
                texture = m_layerTextures[slot][static_cast<std::size_t>(p)];
            }
            f->glBindTexture(GL_TEXTURE_2D, texture);
            m_layerProgram.setUniformValue(("plane" + std::to_string(p)).c_str(), p);
        }
        f->glActiveTexture(GL_TEXTURE3);
        f->glBindTexture(GL_TEXTURE_2D, targets(src).texture);
        m_layerProgram.setUniformValue("accum", 3);
        f->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        m_vao.release();
        m_layerProgram.release();
        m_levels[depth].current = dst;
    }
    return true;
}

void GlCompositor::ensurePrepTarget(PrepTarget& t, int width, int height) {
    auto* f = gl();
    if (t.fbo == 0) {
        f->glGenFramebuffers(1, &t.fbo);
        f->glGenTextures(1, &t.texture);
    }
    if (t.width == width && t.height == height) {
        return;
    }
    f->glBindTexture(GL_TEXTURE_2D, t.texture);
    f->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_HALF_FLOAT, nullptr);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    f->glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
    f->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.texture, 0);
    f->glBindTexture(GL_TEXTURE_2D, 0);
    t.width = width;
    t.height = height;
}

void GlCompositor::setKeyUniforms(QOpenGLShaderProgram& program, const KeyParams& key) {
    program.setUniformValue("keyOn", key.enabled);
    program.setUniformValue("keyColor", QVector3D(key.keyColor[0], key.keyColor[1], key.keyColor[2]));
    program.setUniformValue("keyTolerance", key.tolerance);
    program.setUniformValue("keySoftness", key.softness);
    program.setUniformValue("keyChoke", key.choke);
    program.setUniformValue("keySpill", key.spill);
    program.setUniformValue("keyMatte", key.matte);
}

void GlCompositor::preparePass(const Layer& layer, std::size_t slot) {
    auto* f = gl();
    const auto& frame = *layer.frame;
    const int w = std::max(1, frame.width);
    const int h = std::max(1, frame.height);
    for (auto& t : m_prep) {
        ensurePrepTarget(t, w, h);
    }
    m_vao.bind();
    // 1. Decoded frame -> keyed RGBA (m_prep[0]).
    f->glBindFramebuffer(GL_FRAMEBUFFER, m_prep[0].fbo);
    f->glViewport(0, 0, w, h);
    m_prepareProgram.bind();
    const auto conv = yuvToRgb(frame.matrix, frame.range);
    m_prepareProgram.setUniformValue("yuvMatrix", QMatrix3x3(conv.matrix.data()));
    m_prepareProgram.setUniformValue("yuvOffset", QVector3D(conv.offset[0], conv.offset[1], conv.offset[2]));
    m_prepareProgram.setUniformValue("format", frame.format == media::PixelFormat::Yuv420P ? 0
                                               : frame.format == media::PixelFormat::Nv12  ? 1
                                                                                           : 2);
    setKeyUniforms(m_prepareProgram, layer.key);
    for (int p = 0; p < 3; ++p) {
        f->glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(p));
        f->glBindTexture(GL_TEXTURE_2D, m_layerTextures[slot][static_cast<std::size_t>(p)]);
        m_prepareProgram.setUniformValue(("plane" + std::to_string(p)).c_str(), p);
    }
    f->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    m_prepareProgram.release();
    // 2. Blur (or the sharpen reference) into m_prep[2], through m_prep[1].
    if (layer.blur.active()) {
        const auto taps = blurKernel(layer.blur.kernelSigma());
        std::vector<GLint> offsets;
        std::vector<GLfloat> weights;
        for (const auto& t : taps) {
            offsets.push_back(t.offset);
            weights.push_back(t.weight);
        }
        const bool sharpenOnly = !(layer.blur.sigma > 0.0f);
        const bool horizontal = layer.blur.horizontal || sharpenOnly;
        const bool vertical = layer.blur.vertical || sharpenOnly;
        m_blurProgram.bind();
        m_blurProgram.setUniformValue("tapCount", static_cast<int>(taps.size()));
        m_blurProgram.setUniformValueArray("offsets", offsets.data(), static_cast<int>(offsets.size()));
        m_blurProgram.setUniformValueArray("weights", weights.data(), static_cast<int>(weights.size()), 1);
        m_blurProgram.setUniformValue("source", 0);
        f->glActiveTexture(GL_TEXTURE0);
        // Horizontal: 0 -> 1 (or a copy); vertical: 1 -> 2 (or a copy).
        const auto pass = [&](const PrepTarget& from, const PrepTarget& to, bool on, int dx, int dy) {
            f->glBindFramebuffer(GL_FRAMEBUFFER, to.fbo);
            f->glBindTexture(GL_TEXTURE_2D, from.texture);
            if (on) {
                f->glUniform2i(m_blurProgram.uniformLocation("direction"), dx, dy); // ivec2: no Qt overload
                m_blurProgram.setUniformValue("tapCount", static_cast<int>(taps.size()));
            } else {
                const GLint zero = 0;
                const GLfloat one = 1.0f;
                m_blurProgram.setUniformValueArray("offsets", &zero, 1);
                m_blurProgram.setUniformValueArray("weights", &one, 1, 1);
                m_blurProgram.setUniformValue("tapCount", 1);
            }
            f->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            if (!on) {
                m_blurProgram.setUniformValueArray("offsets", offsets.data(), static_cast<int>(offsets.size()));
                m_blurProgram.setUniformValueArray("weights", weights.data(), static_cast<int>(weights.size()), 1);
            }
        };
        pass(m_prep[0], m_prep[1], horizontal, 1, 0);
        pass(m_prep[1], m_prep[2], vertical, 0, 1);
        m_blurProgram.release();
    }
    m_vao.release();
    f->glBindTexture(GL_TEXTURE_2D, 0);
}

unsigned int GlCompositor::maskTexture(std::size_t index, const MaskImage& mask) {
    auto* f = gl();
    if (m_maskTextures[index] == 0) {
        f->glGenTextures(1, &m_maskTextures[index]);
        f->glBindTexture(GL_TEXTURE_2D, m_maskTextures[index]);
        f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    f->glBindTexture(GL_TEXTURE_2D, m_maskTextures[index]);
    f->glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    f->glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, mask.width, mask.height, 0, GL_RED, GL_UNSIGNED_BYTE, mask.alpha.data());
    return m_maskTextures[index];
}

void GlCompositor::setPhase14Uniforms(const Layer& layer, bool prepared) {
    auto* f = gl();
    auto& p = m_layerProgram;
    // Key in this pass only when the prepass did not apply it.
    KeyParams key = layer.key;
    key.enabled = key.enabled && !prepared;
    setKeyUniforms(p, key);
    p.setUniformValue("blurOn", layer.blur.sigma > 0.0f && (layer.blur.horizontal || layer.blur.vertical));
    p.setUniformValue("sharpen", layer.blur.sharpen);
    // Lumetri.
    const auto& lum = layer.grade.lumetri;
    p.setUniformValue("lumOn", lum.enabled);
    for (std::size_t i = 0; i < 3; ++i) {
        p.setUniformValue(("wheelOffset[" + std::to_string(i) + "]").c_str(),
                          QVector3D(lum.wheelOffset[i][0], lum.wheelOffset[i][1], lum.wheelOffset[i][2]));
    }
    p.setUniformValue("wheelLuma", QVector3D(lum.wheelLuma[0], lum.wheelLuma[1], lum.wheelLuma[2]));
    p.setUniformValue("curvesOn", lum.enabled && lum.curves != nullptr);
    f->glActiveTexture(GL_TEXTURE5);
    if (lum.enabled && lum.curves) {
        if (m_curvesTexture == 0) {
            f->glGenTextures(1, &m_curvesTexture);
            f->glBindTexture(GL_TEXTURE_2D, m_curvesTexture);
            f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        std::vector<float> rgb(256 * 3);
        for (std::size_t i = 0; i < 256; ++i) {
            for (std::size_t c = 0; c < 3; ++c) {
                rgb[i * 3 + c] = lum.curves->rgb[c][i];
            }
        }
        f->glBindTexture(GL_TEXTURE_2D, m_curvesTexture);
        f->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB32F, 256, 1, 0, GL_RGB, GL_FLOAT, rgb.data());
    } else {
        f->glBindTexture(GL_TEXTURE_2D, 0);
    }
    p.setUniformValue("curves", 5);
    const auto& q = lum.hsl;
    p.setUniformValue("hslOn", lum.enabled && q.enabled);
    p.setUniformValue("hslHueCenter", q.hueCenter);
    p.setUniformValue("hslHueRange", q.hueRange);
    p.setUniformValue("hslSoftness", q.softness);
    p.setUniformValue("hslSatMin", q.satMin);
    p.setUniformValue("hslLumaMin", q.lumaMin);
    p.setUniformValue("hslLumaMax", q.lumaMax);
    p.setUniformValue("hslHueShift", q.hueShift);
    p.setUniformValue("hslSaturation", q.saturation);
    p.setUniformValue("hslLuma", q.luma);
    p.setUniformValue("hslShowMask", q.showMask);
    p.setUniformValue("vigAmount", lum.enabled ? lum.vignette.amount : 0.0f);
    p.setUniformValue("vigMidpoint", lum.vignette.midpoint);
    p.setUniformValue("vigFeather", lum.vignette.feather);
    // Masks on units 6..10.
    const std::pair<const char*, const MaskImagePtr*> masks[] = {
        {"maskOpacity", &layer.masks.opacity}, {"maskColor", &layer.masks.color}, {"maskLumetri", &layer.masks.lumetri},
        {"maskLut", &layer.masks.lut},         {"maskBlur", &layer.masks.blur}};
    for (std::size_t i = 0; i < 5; ++i) {
        const auto& mask = *masks[i].second;
        f->glActiveTexture(GL_TEXTURE6 + static_cast<GLenum>(i));
        if (mask && mask->width > 0) {
            maskTexture(i, *mask);
        } else {
            f->glBindTexture(GL_TEXTURE_2D, 0);
        }
        p.setUniformValue(masks[i].first, 6 + static_cast<int>(i));
        p.setUniformValue((std::string(masks[i].first) + "On").c_str(), mask && mask->width > 0);
    }
    f->glActiveTexture(GL_TEXTURE0);
}

unsigned int GlCompositor::lutTextureFor(const std::shared_ptr<const Lut3D>& lut) {
    auto& entry = m_luts[lut.get()];
    entry.usedThisFrame = true;
    if (entry.lut != lut) { // new (or a different LUT at a reused address)
        auto* f = gl();
        if (entry.texture == 0) {
            f->glGenTextures(1, &entry.texture);
        }
        entry.lut = lut;
        f->glBindTexture(GL_TEXTURE_3D, entry.texture);
        f->glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        f->glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        f->glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        f->glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        f->glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
        f->glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        f->glTexImage3D(GL_TEXTURE_3D, 0, GL_RGB16F, lut->size, lut->size, lut->size, 0, GL_RGB, GL_FLOAT,
                        lut->data.data());
    }
    return entry.texture;
}

void GlCompositor::present(unsigned int targetFramebuffer, int viewportWidth, int viewportHeight) {
    auto* f = gl();
    f->glBindFramebuffer(GL_FRAMEBUFFER, targetFramebuffer);
    f->glViewport(0, 0, viewportWidth, viewportHeight);
    f->glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    f->glClear(GL_COLOR_BUFFER_BIT);
    if (!m_initialized || m_width <= 0 || m_levels.empty() || viewportWidth <= 0 || viewportHeight <= 0) {
        return;
    }
    const auto r = fitRect(static_cast<float>(m_canvasAspect * 1000.0), 1000.0f, static_cast<float>(viewportWidth),
                           static_cast<float>(viewportHeight));
    const float vw = static_cast<float>(viewportWidth);
    const float vh = static_cast<float>(viewportHeight);
    m_presentProgram.bind();
    m_presentProgram.setUniformValue("rect", QVector4D(r[0] / vw * 2.0f - 1.0f, 1.0f - (r[1] + r[3]) / vh * 2.0f,
                                                       r[2] / vw * 2.0f, r[3] / vh * 2.0f));
    f->glActiveTexture(GL_TEXTURE0);
    const auto& top = m_levels.front();
    f->glBindTexture(GL_TEXTURE_2D, top.targets[static_cast<std::size_t>(top.current)].texture);
    m_presentProgram.setUniformValue("image", 0);
    m_vao.bind();
    f->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    m_vao.release();
    m_presentProgram.release();
}

QImage GlCompositor::readback() {
    if (!m_initialized || m_width <= 0 || m_levels.empty()) {
        return {};
    }
    auto* f = gl();
    QImage image(m_width, m_height, QImage::Format_RGBA8888);
    GLint previousFbo = 0;
    f->glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFbo);
    const auto& top = m_levels.front();
    f->glBindFramebuffer(GL_FRAMEBUFFER, top.targets[static_cast<std::size_t>(top.current)].fbo);
    f->glPixelStorei(GL_PACK_ALIGNMENT, 1);
    f->glReadPixels(0, 0, m_width, m_height, GL_RGBA, GL_UNSIGNED_BYTE, image.bits());
    f->glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previousFbo));
    return image.mirrored(false, true); // GL rows are bottom-up
}

void GlCompositor::release() {
    if (!m_initialized) {
        return;
    }
    auto* f = gl();
    for (auto& level : m_levels) {
        for (auto& t : level.targets) {
            if (t.fbo != 0) {
                f->glDeleteFramebuffers(1, &t.fbo);
                f->glDeleteTextures(1, &t.texture);
            }
        }
    }
    m_levels.clear();
    for (auto& t : m_prep) {
        if (t.fbo != 0) {
            f->glDeleteFramebuffers(1, &t.fbo);
            f->glDeleteTextures(1, &t.texture);
        }
        t = {};
    }
    for (auto& t : m_maskTextures) {
        if (t != 0) {
            f->glDeleteTextures(1, &t);
        }
        t = 0;
    }
    if (m_curvesTexture != 0) {
        f->glDeleteTextures(1, &m_curvesTexture);
        m_curvesTexture = 0;
    }
    m_prepareProgram.removeAllShaders();
    m_blurProgram.removeAllShaders();
    for (auto& tex : m_layerTextures) {
        f->glDeleteTextures(3, tex.data());
    }
    m_layerTextures.clear();
    for (auto& [key, entry] : m_luts) {
        f->glDeleteTextures(1, &entry.texture);
    }
    m_luts.clear();
    m_vbo.destroy();
    m_vao.destroy();
    m_layerProgram.removeAllShaders();
    m_presentProgram.removeAllShaders();
    m_width = 0;
    m_height = 0;
    m_initialized = false;
}

} // namespace loki::render
