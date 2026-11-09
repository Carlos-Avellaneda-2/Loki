#pragma once

#include "render/composite.h"

#include <cstdint>
#include <vector>

namespace loki::render {

// Software compositor (D22): same math as GlCompositor (affine layers, YUV->RGB, grading, wipe,
// blend modes, straight alpha of RGBA layers) with nearest-neighbour sampling. Slow but needs no
// GPU: export fallback without OpenGL 3.3 and the reference for tests on CI.
struct CpuImage {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba; // width * height * 4, rows top to bottom, alpha = 255

    [[nodiscard]] std::array<int, 3> pixel(int x, int y) const {
        const auto i = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) * 4;
        return {rgba[i], rgba[i + 1], rgba[i + 2]};
    }
};

// Composites `frame` (canvas frame.width x frame.height) scaled to width x height.
[[nodiscard]] CpuImage compositeCpu(const CompositeFrame& frame, int width, int height);

// Color of a frame at pixel (x, y) of its decoded planes, plus alpha (1 unless RGBA).
struct SampledColor {
    Rgb rgb;
    float alpha = 1.0f;
};
[[nodiscard]] SampledColor sampleFrame(const media::VideoFrame& frame, int x, int y) noexcept;

} // namespace loki::render
