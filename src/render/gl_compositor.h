#pragma once

#include "render/composite.h"

#include <QImage>
#include <QOpenGLBuffer>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>

#include <array>
#include <map>
#include <vector>

namespace loki::render {

// OpenGL 3.3 multitrack compositor (D11). Renders a CompositeFrame into an internal RGBA16F
// framebuffer (ping-pong: each layer reads the accumulated image to apply its blend mode), then
// presents it letterboxed. The same class renders offscreen for export (Phase 10).
// All methods require the owning context to be current.
class GlCompositor {
public:
    GlCompositor() = default;
    ~GlCompositor() = default; // call release() with the context current before destruction
    GlCompositor(const GlCompositor&) = delete;
    GlCompositor& operator=(const GlCompositor&) = delete;
    GlCompositor(GlCompositor&&) = delete;
    GlCompositor& operator=(GlCompositor&&) = delete;

    bool initialize();
    [[nodiscard]] bool ready() const noexcept { return m_initialized; }

    // Composites `frame` at `width` x `height` pixels (the canvas is scaled to that size; use a
    // smaller size than the sequence for preview quality).
    void render(const CompositeFrame& frame, int width, int height);
    // Draws the last result letterboxed into the bound framebuffer `target` of the given size.
    void present(unsigned int targetFramebuffer, int viewportWidth, int viewportHeight);
    // Last result as 8-bit RGBA (tests, export).
    [[nodiscard]] QImage readback();
    void release();

    [[nodiscard]] int width() const noexcept { return m_width; }
    [[nodiscard]] int height() const noexcept { return m_height; }
    [[nodiscard]] double canvasAspect() const noexcept { return m_canvasAspect; }

private:
    struct Target {
        unsigned int fbo = 0;
        unsigned int texture = 0;
    };
    // Ping-pong pair of one nesting level (0 = the result; 1.. = nested sequences, D30).
    struct Level {
        std::array<Target, 2> targets{};
        int width = 0;
        int height = 0;
        int current = 0; // index of the target holding the accumulated image
    };

    // Prepass targets (keyed source, blur pass, blurred source) at the decoded frame's size.
    struct PrepTarget {
        unsigned int fbo = 0;
        unsigned int texture = 0;
        int width = 0;
        int height = 0;
    };

    Level& ensureLevel(std::size_t depth, int width, int height);
    void ensurePrepTarget(PrepTarget& target, int width, int height);
    // Key and blur/sharpen of a frame layer (uploaded in `slot`) into m_prep (D35, D37).
    void preparePass(const Layer& layer, std::size_t slot);
    static void setKeyUniforms(QOpenGLShaderProgram& program, const KeyParams& key);
    // Lumetri, key, masks, blur mix uniforms and textures of the layer pass.
    void setPhase14Uniforms(const Layer& layer, bool prepared);
    unsigned int maskTexture(std::size_t index, const MaskImage& mask);
    // Composites `frame` into level `depth`; returns false past the nesting limit.
    bool renderLevel(const CompositeFrame& frame, int width, int height, std::size_t depth);
    void uploadLayer(std::size_t slot, const media::VideoFrame& frame);
    unsigned int lutTextureFor(const std::shared_ptr<const Lut3D>& lut);

    struct LutTexture {
        std::shared_ptr<const Lut3D> lut;
        unsigned int texture = 0;
        bool usedThisFrame = false;
    };
    std::map<const Lut3D*, LutTexture> m_luts;

    QOpenGLShaderProgram m_layerProgram;
    QOpenGLShaderProgram m_presentProgram;
    QOpenGLShaderProgram m_prepareProgram;
    QOpenGLShaderProgram m_blurProgram;
    std::array<PrepTarget, 3> m_prep{};
    std::array<unsigned int, 5> m_maskTextures{};
    unsigned int m_curvesTexture = 0;
    QOpenGLVertexArrayObject m_vao;
    QOpenGLBuffer m_vbo{QOpenGLBuffer::VertexBuffer};
    std::vector<Level> m_levels;
    std::size_t m_nextSlot = 0; // layer texture slot of the next frame layer (whole render)
    int m_width = 0;
    int m_height = 0;
    double m_canvasAspect = 16.0 / 9.0;
    std::vector<std::array<unsigned int, 3>> m_layerTextures;
    bool m_initialized = false;
};

} // namespace loki::render
