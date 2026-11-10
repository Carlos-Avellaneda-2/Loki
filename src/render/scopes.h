#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace loki::render {

// Video scopes (D32): statistics of a picture for the Lumetri Scopes panel. Pure functions so the
// panel can run them on a worker thread and tests can check them without a GPU.
struct ScopeData {
    static constexpr int kLevels = 256; // vertical resolution of waveforms / histogram bins
    int columns = 0;                    // waveform columns (picture width, reduced)
    // Waveforms: columns x kLevels hit counts, index = level * columns + column (level 0 = black).
    std::vector<std::uint32_t> luma;
    std::array<std::vector<std::uint32_t>, 3> parade; // R, G, B
    // Vectorscope: kLevels x kLevels counts, index = cr * kLevels + cb (128 = neutral).
    std::vector<std::uint32_t> vectorscope;
    // Histograms of R, G, B and luma.
    std::array<std::array<std::uint32_t, kLevels>, 4> histogram{};
    std::uint64_t samples = 0;
};

// Rec.709 luma and color difference of an 8-bit RGB pixel (0..255 each; cb/cr centered at 128).
struct YCbCr {
    int y = 0;
    int cb = 128;
    int cr = 128;
};
[[nodiscard]] YCbCr toYCbCr709(int r, int g, int b) noexcept;

// Analyzes an RGBA8 image (rows top to bottom, `stride` bytes per row). Waveforms get at most
// `maxColumns` columns; every pixel is counted.
[[nodiscard]] ScopeData analyzeScopes(const std::uint8_t* rgba, int width, int height, int stride, int maxColumns = 512);

} // namespace loki::render
