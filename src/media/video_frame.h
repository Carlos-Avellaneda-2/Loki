#pragma once

#include "util/rational.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace loki::media {

// CPU pixel layouts produced by the decoders. The GPU renderer converts YUV to RGB in a shader.
enum class PixelFormat {
    Yuv420P, // 3 planes: Y, U, V (U/V at half width and height)
    Nv12,    // 2 planes: Y, interleaved UV (typical hardware decoder output)
    Rgba,    // 1 plane, 8 bits per channel (thumbnails)
};

enum class ColorMatrix { Bt601, Bt709 };
enum class ColorRange { Limited, Full };

// One decoded picture in system memory. Immutable once published (shared between threads).
struct VideoFrame {
    PixelFormat format = PixelFormat::Yuv420P;
    int width = 0;
    int height = 0;
    ColorMatrix matrix = ColorMatrix::Bt709;
    ColorRange range = ColorRange::Limited;
    Rational pts;      // presentation time in the source (seconds)
    Rational duration; // nominal frame duration (0 if unknown)
    int planeCount = 0;
    std::array<int, 3> strides{};      // bytes per row of each plane
    std::array<int, 3> planeHeights{}; // rows of each plane
    std::array<std::vector<std::uint8_t>, 3> planes;

    [[nodiscard]] std::size_t byteSize() const noexcept {
        return planes[0].size() + planes[1].size() + planes[2].size();
    }
    // Contains `t` when pts <= t < pts + duration.
    [[nodiscard]] bool contains(const Rational& t) const { return pts <= t && t < pts + duration; }
};

using VideoFramePtr = std::shared_ptr<const VideoFrame>;

} // namespace loki::media
