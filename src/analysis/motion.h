#pragma once

#include "media/video_frame.h"
#include "model/mask.h"
#include "render/effects.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <vector>

namespace loki::analysis {

// 8-bit luma picture used by the analyses (reduced resolution).
struct GrayImage {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> pixels;

    [[nodiscard]] std::uint8_t at(int x, int y) const noexcept {
        return pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)];
    }
};

// Luma of a decoded frame, box-downscaled so its width is at most `maxWidth`.
[[nodiscard]] GrayImage lumaOf(const media::VideoFrame& frame, int maxWidth);

// Global motion from `previous` to `current` (D36): block matching on a grid, then a robust least
// squares similarity fit (translation, rotation, uniform scale). Translations are in the images'
// pixels.
[[nodiscard]] render::FrameMotion estimateMotion(const GrayImage& previous, const GrayImage& current,
                                                 int searchRadius = 8);

// Where a region moved from `previous` to `current` (D34 mask tracking): best translation (and
// scale among 0.96 / 1 / 1.04 when `withScale`) of the region's texture, by sum of absolute
// differences. Region in pixels of the images.
struct RegionMotion {
    double dx = 0.0;
    double dy = 0.0;
    double scale = 1.0;
};
[[nodiscard]] RegionMotion trackRegion(const GrayImage& previous, const GrayImage& current, double x, double y,
                                       double width, double height, int searchRadius, bool withScale);

// Progress callback: 0..1. Returning false from `cancel` stops; jobs return nullopt when stopped.
using Progress = std::function<void(double)>;

// Warp Stabilizer analysis of media [from, to) (source time): the motion of every frame, measured at
// a reduced size and scaled back to media pixels. Decodes on the calling thread.
[[nodiscard]] std::optional<render::StabilizerAnalysis> analyzeStabilizer(const std::filesystem::path& path,
                                                                          const Rational& from, const Rational& to,
                                                                          const Progress& progress,
                                                                          const std::atomic<bool>& cancel);

// Mask tracking (D34): follows the mask's region frame by frame from `from` towards `to` (forwards
// or backwards) and returns one path key per frame (media pixels, source time).
[[nodiscard]] std::optional<std::vector<model::MaskPathKey>> trackMask(const std::filesystem::path& path,
                                                                       const std::vector<model::MaskVertex>& start,
                                                                       const Rational& from, const Rational& to,
                                                                       const Progress& progress,
                                                                       const std::atomic<bool>& cancel);

} // namespace loki::analysis
