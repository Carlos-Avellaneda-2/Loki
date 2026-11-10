#pragma once

#include "media/video_frame.h"
#include "model/effect_registry.h"
#include "render/effects.h"
#include "render/grading.h"

#include <array>
#include <memory>
#include <vector>

namespace loki::render {

using model::BlendMode;

struct CompositeFrame;

// One video layer of a composite, already resolved for a time (D11: no OpenGL here, so the same
// description feeds the preview compositor and, in Phase 10, the export).
struct Layer {
    media::VideoFramePtr frame;
    // Size of the source in its own pixels (original media size, independent of the decoded size
    // chosen by playback quality).
    double sourceWidth = 0;
    double sourceHeight = 0;
    model::ClipTransform transform;
    LayerGrade grade; // color correction, Lumetri, LUT, dip to black/white
    WipeMask wipe;    // wipe / iris transition mask (canvas space)
    // Phase 14: Ultra Key (D35), blur/sharpen (D37), per-stage masks (D34) and the stabilizer
    // correction (D36, source pixels -> source pixels, applied before Motion).
    KeyParams key;
    BlurParams blur;
    LayerMasks masks;
    Matrix3 pre = identityMatrix();
    // Nested sequence (D30): the picture is this composite (transparent where empty), rendered at
    // sourceWidth x sourceHeight; `frame` is unused.
    std::shared_ptr<const CompositeFrame> nested;
    // Adjustment layer (D31): the picture is the composite below this layer, graded; covers the
    // whole canvas (transform position/scale ignored, opacity and blend mode apply).
    bool adjustment = false;

    [[nodiscard]] bool hasPicture() const noexcept {
        return adjustment || nested != nullptr || (frame && frame->planeCount > 0);
    }
};

// Layers bottom (index 0, V1) to top, composited over black in a width x height canvas.
struct CompositeFrame {
    int width = 0;
    int height = 0;
    std::vector<Layer> layers;
    // Nested composites start transparent (layers below show through, D30); the top level starts
    // opaque black.
    bool transparent = false;
};

using CompositePtr = std::shared_ptr<const CompositeFrame>;

// Row-major 3x3 affine matrix mapping layer-local pixels (0..sourceWidth, 0..sourceHeight, y down)
// to canvas pixels (y down): translate(position) * rotate(rotation) * scale(scale/100) * translate(-anchor).
[[nodiscard]] std::array<double, 9> layerToCanvas(const Layer& layer) noexcept;

// Applies a row-major 3x3 affine matrix to a point.
[[nodiscard]] std::array<double, 2> transformPoint(const std::array<double, 9>& m, double x, double y) noexcept;

// Canvas-space corners of a layer: top-left, top-right, bottom-left, bottom-right.
[[nodiscard]] std::array<std::array<double, 2>, 4> layerCorners(const Layer& layer) noexcept;

using Rgb = std::array<float, 3>;

// Blend formulas (gamma space, values 0..1), identical to the GPU shader. Returns the blended
// colour before opacity is applied.
[[nodiscard]] Rgb blendColor(BlendMode mode, const Rgb& src, const Rgb& dst) noexcept;
// Full per-pixel composite step: mix(dst, blend(src, dst), opacity).
[[nodiscard]] Rgb compositePixel(BlendMode mode, const Rgb& src, const Rgb& dst, float opacity) noexcept;

// Straight-alpha color.
struct Rgba {
    Rgb rgb{0.0f, 0.0f, 0.0f};
    float alpha = 0.0f;
};
// Composite step over a destination with alpha (nested sequences, D30): the blend mode only
// applies where the destination is opaque, then `src` (coverage `alpha`) goes over it. With an
// opaque destination this equals compositePixel. Identical to the GPU shader.
[[nodiscard]] Rgba compositeOver(BlendMode mode, const Rgb& src, float alpha, const Rgba& dst) noexcept;

// Canvas layer of an adjustment layer: identity transform covering width x height.
void coverCanvas(Layer& layer, int width, int height) noexcept;

} // namespace loki::render
