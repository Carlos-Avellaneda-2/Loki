#pragma once

#include "media/video_frame.h"

#include <QOpenGLBuffer>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>

#include <array>
#include <memory>

namespace loki::render {

// Draws one VideoFrame with OpenGL 3.3 core: planes are uploaded as textures (R8 / RG8 / RGBA8)
// and converted YUV->RGB in the fragment shader, letterboxed into the viewport.
// All methods require the owning OpenGL context to be current.
// Phase 6 grows this into the multitrack compositor behind a RenderBackend interface.
class GlFrameRenderer {
public:
    GlFrameRenderer() = default;
    ~GlFrameRenderer() = default; // call release() with the context current before destruction
    GlFrameRenderer(const GlFrameRenderer&) = delete;
    GlFrameRenderer& operator=(const GlFrameRenderer&) = delete;
    GlFrameRenderer(GlFrameRenderer&&) = delete;
    GlFrameRenderer& operator=(GlFrameRenderer&&) = delete;

    bool initialize();
    void upload(const media::VideoFrame& frame);
    // Clears to black and draws the last uploaded frame fitted in the viewport (pixels).
    void draw(int viewportWidth, int viewportHeight);
    void release();

    [[nodiscard]] bool hasFrame() const noexcept { return m_frameWidth > 0; }

private:
    QOpenGLShaderProgram m_program;
    QOpenGLVertexArrayObject m_vao;
    QOpenGLBuffer m_vbo{QOpenGLBuffer::VertexBuffer};
    std::array<unsigned int, 3> m_textures{};
    std::array<std::array<int, 2>, 3> m_textureSizes{};
    media::PixelFormat m_format = media::PixelFormat::Yuv420P;
    media::ColorMatrix m_matrix = media::ColorMatrix::Bt709;
    media::ColorRange m_range = media::ColorRange::Limited;
    int m_frameWidth = 0;
    int m_frameHeight = 0;
    bool m_initialized = false;
};

} // namespace loki::render
