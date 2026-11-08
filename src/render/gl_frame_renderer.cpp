#include "render/gl_frame_renderer.h"

#include "render/color_conversion.h"

#include <QMatrix3x3>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>

#include <spdlog/spdlog.h>

namespace loki::render {

namespace {

constexpr auto kVertexShader = R"(#version 330 core
layout(location = 0) in vec2 position;
layout(location = 1) in vec2 texCoord;
uniform vec4 rect; // x, y, w, h in normalized device coordinates
out vec2 uv;
void main() {
    uv = texCoord;
    gl_Position = vec4(rect.xy + position * rect.zw, 0.0, 1.0);
}
)";

constexpr auto kFragmentShader = R"(#version 330 core
in vec2 uv;
out vec4 color;
uniform int format;      // 0 = YUV420P, 1 = NV12, 2 = RGBA
uniform sampler2D plane0;
uniform sampler2D plane1;
uniform sampler2D plane2;
uniform mat3 yuvMatrix;  // column-major (GL), see color_conversion.cpp
uniform vec3 yuvOffset;
void main() {
    if (format == 2) {
        color = vec4(texture(plane0, uv).rgb, 1.0);
        return;
    }
    float y = texture(plane0, uv).r;
    vec2 c = format == 1 ? texture(plane1, uv).rg : vec2(texture(plane1, uv).r, texture(plane2, uv).r);
    vec3 rgb = yuvMatrix * vec3(y, c.x, c.y) + yuvOffset;
    color = vec4(clamp(rgb, 0.0, 1.0), 1.0);
}
)";

// Unit quad: position (0..1) and texture coordinate (top row of the image at the top).
constexpr float kQuad[] = {
    0.0f, 0.0f, 0.0f, 1.0f, //
    1.0f, 0.0f, 1.0f, 1.0f, //
    0.0f, 1.0f, 0.0f, 0.0f, //
    1.0f, 1.0f, 1.0f, 0.0f, //
};

} // namespace

bool GlFrameRenderer::initialize() {
    if (m_initialized) {
        return true;
    }
    if (!m_program.addShaderFromSourceCode(QOpenGLShader::Vertex, kVertexShader) ||
        !m_program.addShaderFromSourceCode(QOpenGLShader::Fragment, kFragmentShader) || !m_program.link()) {
        spdlog::error("Video shader failed: {}", m_program.log().toStdString());
        return false;
    }
    auto* f = QOpenGLContext::currentContext()->extraFunctions();
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
    f->glGenTextures(3, m_textures.data());
    for (const auto tex : m_textures) {
        f->glBindTexture(GL_TEXTURE_2D, tex);
        f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    f->glBindTexture(GL_TEXTURE_2D, 0);
    m_initialized = true;
    return true;
}

void GlFrameRenderer::upload(const media::VideoFrame& frame) {
    if (!m_initialized || frame.planeCount == 0) {
        return;
    }
    auto* f = QOpenGLContext::currentContext()->extraFunctions();
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
        const int width = frame.strides[p] / bytesPerPixel; // planes are tightly packed
        const int height = frame.planeHeights[p];
        f->glBindTexture(GL_TEXTURE_2D, m_textures[p]);
        if (m_textureSizes[p][0] != width || m_textureSizes[p][1] != height || frame.format != m_format) {
            f->glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(internal), width, height, 0, format, GL_UNSIGNED_BYTE,
                            frame.planes[p].data());
            m_textureSizes[p] = {width, height};
        } else {
            f->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, format, GL_UNSIGNED_BYTE, frame.planes[p].data());
        }
    }
    f->glBindTexture(GL_TEXTURE_2D, 0);
    m_format = frame.format;
    m_matrix = frame.matrix;
    m_range = frame.range;
    m_frameWidth = frame.width;
    m_frameHeight = frame.height;
}

void GlFrameRenderer::draw(int viewportWidth, int viewportHeight) {
    auto* f = QOpenGLContext::currentContext()->extraFunctions();
    f->glViewport(0, 0, viewportWidth, viewportHeight);
    f->glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    f->glClear(GL_COLOR_BUFFER_BIT);
    if (!m_initialized || !hasFrame() || viewportWidth <= 0 || viewportHeight <= 0) {
        return;
    }
    const auto r = fitRect(static_cast<float>(m_frameWidth), static_cast<float>(m_frameHeight),
                           static_cast<float>(viewportWidth), static_cast<float>(viewportHeight));
    const float vw = static_cast<float>(viewportWidth);
    const float vh = static_cast<float>(viewportHeight);
    // Pixel rect (origin top-left) -> NDC (origin bottom-left).
    const float x = r[0] / vw * 2.0f - 1.0f;
    const float y = 1.0f - (r[1] + r[3]) / vh * 2.0f;

    const auto conv = yuvToRgb(m_matrix, m_range);
    // Row-major 3x3 -> QMatrix3x3 (row-major constructor; Qt uploads it column-major).
    const QMatrix3x3 matrix(conv.matrix.data());

    m_program.bind();
    m_program.setUniformValue("rect", QVector4D(x, y, r[2] / vw * 2.0f, r[3] / vh * 2.0f));
    m_program.setUniformValue("format", m_format == media::PixelFormat::Yuv420P ? 0
                                        : m_format == media::PixelFormat::Nv12  ? 1
                                                                                 : 2);
    m_program.setUniformValue("yuvMatrix", matrix);
    m_program.setUniformValue("yuvOffset", QVector3D(conv.offset[0], conv.offset[1], conv.offset[2]));
    for (int i = 0; i < 3; ++i) {
        f->glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(i));
        f->glBindTexture(GL_TEXTURE_2D, m_textures[static_cast<std::size_t>(i)]);
        m_program.setUniformValue(("plane" + std::to_string(i)).c_str(), i);
    }
    m_vao.bind();
    f->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    m_vao.release();
    f->glActiveTexture(GL_TEXTURE0);
    m_program.release();
}

void GlFrameRenderer::release() {
    if (!m_initialized) {
        return;
    }
    auto* f = QOpenGLContext::currentContext()->extraFunctions();
    f->glDeleteTextures(3, m_textures.data());
    m_vbo.destroy();
    m_vao.destroy();
    m_program.removeAllShaders();
    m_textures = {};
    m_textureSizes = {};
    m_frameWidth = 0;
    m_frameHeight = 0;
    m_initialized = false;
}

} // namespace loki::render
