#pragma once

#include "media/video_frame.h"
#include "model/effect_registry.h"

#include <array>
#include <memory>
#include <vector>

namespace loki::render {

using model::BlendMode;

// One video layer of a composite, already resolved for a time (D11: no OpenGL here, so the same
// description feeds the preview compositor and, in Phase 10, the export).
struct Layer {
    media::VideoFramePtr frame;
    // Size of the source in its own pixels (original media size, independent of the decoded size
    // chosen by playback quality).
    double sourceWidth = 0;
    double sourceHeight = 0;
    model::ClipTransform transform;
};

// Layers bottom (index 0, V1) to top, composited over black in a width x height canvas.
struct CompositeFrame {
    int width = 0;
    int height = 0;
    std::vector<Layer> layers;
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

} // namespace loki::render
