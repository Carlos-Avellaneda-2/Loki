#pragma once

#include "media/video_frame.h"

#include <array>

namespace loki::render {

// rgb = matrix * (y, u, v) + offset, with y/u/v and rgb normalized to [0, 1].
// Shared by the GPU shader (uniforms) and the CPU reference used in tests/export.
struct YuvToRgb {
    std::array<float, 9> matrix{}; // row-major 3x3
    std::array<float, 3> offset{};
};

[[nodiscard]] YuvToRgb yuvToRgb(media::ColorMatrix matrix, media::ColorRange range) noexcept;

// CPU reference (clamped to [0, 1]).
[[nodiscard]] std::array<float, 3> convertPixel(const YuvToRgb& c, float y, float u, float v) noexcept;

// Fits a source rectangle into a viewport keeping aspect (letterbox/pillarbox). Returns
// x, y, width, height in viewport pixels.
[[nodiscard]] std::array<float, 4> fitRect(float srcWidth, float srcHeight, float dstWidth, float dstHeight) noexcept;

} // namespace loki::render
