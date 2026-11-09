#include "render/gl_compositor.h"

#include "render/color_conversion.h"

#include <QMatrix3x3>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>

#include <spdlog/spdlog.h>

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
uniform int format;      // 0 = YUV420P, 1 = NV12, 2 = RGBA
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
// Wipe mask (render::wipeAlpha).
uniform bool wipeOn;
uniform float wipeProgress;
uniform float wipeSoftness;
uniform bool wipeReverse;

vec3 sourceColor() {
    if (format == 2) {
        return texture(plane0, uv).rgb;
    }
    float y = texture(plane0, uv).r;
    vec2 c = format == 1 ? texture(plane1, uv).rg : vec2(texture(plane1, uv).r, texture(plane2, uv).r);
    return clamp(yuvMatrix * vec3(y, c.x, c.y) + yuvOffset, 0.0, 1.0);
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

vec3 grade(vec3 c) {
    if (colorOn) {
        c = (c * exp2(exposure) - 0.5) * (1.0 + contrast / 100.0) + 0.5;
        float luma = dot(c, vec3(0.2126, 0.7152, 0.0722));
        c = luma + (c - luma) * (saturation / 100.0);
        c.r *= 1.0 + 0.1 * temperature / 100.0;
        c.b *= 1.0 - 0.1 * temperature / 100.0;
        c = clamp(c, 0.0, 1.0);
    }
    if (lutOn && lutMix > 0.0) {
        vec3 x = clamp((c - lutMin) / max(lutMax - lutMin, vec3(1e-6)), 0.0, 1.0);
        vec3 l = texture(lut, (x * (lutSize - 1.0) + 0.5) / lutSize).rgb;
        c = mix(c, l, clamp(lutMix, 0.0, 1.0));
    }
    return c * (1.0 - clamp(dim, 0.0, 1.0));
}

float wipeAlpha(float x) {
    if (!wipeOn) return 1.0;
    float soft = max(wipeSoftness, 1e-4);
    float edge = -soft + wipeProgress * (1.0 + 2.0 * soft);
    float t = clamp((edge - x) / soft + 0.5, 0.0, 1.0);
    float shown = t * t * (3.0 - 2.0 * t);
    return wipeReverse ? 1.0 - shown : shown;
}

void main() {
    vec3 dst = texture(accum, gl_FragCoord.xy / targetSize).rgb;
    vec3 src = grade(sourceColor());
    float srcAlpha = format == 2 ? texture(plane0, uv).a : 1.0;
    float a = clamp(opacity, 0.0, 1.0) * srcAlpha * wipeAlpha(gl_FragCoord.x / targetSize.x);
    color = vec4(mix(dst, blend(src, dst), a), 1.0);
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
    color = vec4(texture(image, uv).rgb, 1.0);
}
)";

// Unit quad (position 0..1, y down in layer pixels; texture coordinate matches).
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
    if (!build(m_layerProgram, kLayerVertex, kLayerFragment) || !build(m_presentProgram, kPresentVertex, kPresentFragment)) {
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

void GlCompositor::ensureTargets(int width, int height) {
    if (width == m_width && height == m_height && m_targets[0].fbo != 0) {
        return;
    }
    auto* f = gl();
    for (auto& t : m_targets) {
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
    m_width = width;
    m_height = height;
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
    ensureTargets(width, height);
    m_canvasAspect = frame.height > 0 ? static_cast<double>(frame.width) / frame.height : 16.0 / 9.0;

    GLint previousFbo = 0;
    f->glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFbo);
    m_current = 0;
    f->glBindFramebuffer(GL_FRAMEBUFFER, m_targets[0].fbo);
    f->glViewport(0, 0, width, height);
    f->glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    f->glClear(GL_COLOR_BUFFER_BIT);

    m_layerProgram.bind();
    m_vao.bind();
    m_layerProgram.setUniformValue("targetSize", QVector2D(static_cast<float>(width), static_cast<float>(height)));
    for (std::size_t i = 0; i < frame.layers.size(); ++i) {
        const auto& layer = frame.layers[i];
        if (!layer.frame || layer.frame->planeCount == 0 || layer.transform.opacity <= 0.0) {
            continue;
        }
        uploadLayer(i, *layer.frame);

        // Copy the accumulated image to the other target, then draw the layer into it.
        const int src = m_current;
        const int dst = 1 - m_current;
        f->glBindFramebuffer(GL_READ_FRAMEBUFFER, m_targets[static_cast<std::size_t>(src)].fbo);
        f->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_targets[static_cast<std::size_t>(dst)].fbo);
        f->glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        f->glBindFramebuffer(GL_FRAMEBUFFER, m_targets[static_cast<std::size_t>(dst)].fbo);

        // layer pixels -> canvas pixels -> NDC (canvas y down, NDC y up).
        const auto m = layerToCanvas(layer);
        const double sx = layer.sourceWidth;
        const double sy = layer.sourceHeight;
        const double cw = frame.width;
        const double ch = frame.height;
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
        const auto conv = yuvToRgb(layer.frame->matrix, layer.frame->range);
        m_layerProgram.setUniformValue("yuvMatrix", QMatrix3x3(conv.matrix.data()));
        m_layerProgram.setUniformValue("yuvOffset", QVector3D(conv.offset[0], conv.offset[1], conv.offset[2]));
        m_layerProgram.setUniformValue("format", layer.frame->format == media::PixelFormat::Yuv420P ? 0
                                                 : layer.frame->format == media::PixelFormat::Nv12  ? 1
                                                                                                    : 2);
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
        for (int p = 0; p < 3; ++p) {
            f->glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(p));
            f->glBindTexture(GL_TEXTURE_2D, m_layerTextures[i][static_cast<std::size_t>(p)]);
            m_layerProgram.setUniformValue(("plane" + std::to_string(p)).c_str(), p);
        }
        f->glActiveTexture(GL_TEXTURE3);
        f->glBindTexture(GL_TEXTURE_2D, m_targets[static_cast<std::size_t>(src)].texture);
        m_layerProgram.setUniformValue("accum", 3);
        f->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        m_current = dst;
    }
    m_vao.release();
    m_layerProgram.release();
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
    if (!m_initialized || m_width <= 0 || viewportWidth <= 0 || viewportHeight <= 0) {
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
    f->glBindTexture(GL_TEXTURE_2D, m_targets[static_cast<std::size_t>(m_current)].texture);
    m_presentProgram.setUniformValue("image", 0);
    m_vao.bind();
    f->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    m_vao.release();
    m_presentProgram.release();
}

QImage GlCompositor::readback() {
    if (!m_initialized || m_width <= 0) {
        return {};
    }
    auto* f = gl();
    QImage image(m_width, m_height, QImage::Format_RGBA8888);
    GLint previousFbo = 0;
    f->glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFbo);
    f->glBindFramebuffer(GL_FRAMEBUFFER, m_targets[static_cast<std::size_t>(m_current)].fbo);
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
    for (auto& t : m_targets) {
        if (t.fbo != 0) {
            f->glDeleteFramebuffers(1, &t.fbo);
            f->glDeleteTextures(1, &t.texture);
        }
        t = {};
    }
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
