#pragma once

#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace loki::render {

using Rgb = std::array<float, 3>;

// Color Correction effect (loki.color), applied in gamma space in this order: exposure (stops),
// contrast around mid gray, saturation (Rec.709 luma), temperature (warm = more red, less blue).
struct ColorAdjust {
    float exposure = 0.0f;    // -4 .. 4 stops
    float contrast = 0.0f;    // -100 .. 100
    float saturation = 100.0f; // 0 .. 200 %
    float temperature = 0.0f; // -100 .. 100

    [[nodiscard]] bool isIdentity() const noexcept {
        return exposure == 0.0f && contrast == 0.0f && saturation == 100.0f && temperature == 0.0f;
    }
};
[[nodiscard]] Rgb adjustColor(const ColorAdjust& adjust, const Rgb& c) noexcept;

// 3D LUT from an Adobe/Resolve .cube file (LUT_3D_SIZE, optional DOMAIN_MIN/MAX).
struct Lut3D {
    int size = 0;
    Rgb domainMin{0.0f, 0.0f, 0.0f};
    Rgb domainMax{1.0f, 1.0f, 1.0f};
    std::vector<float> data; // size^3 RGB triplets, red changes fastest (the .cube order)

    // Trilinear lookup.
    [[nodiscard]] Rgb sample(const Rgb& c) const noexcept;
};
// Parses .cube text. Throws std::runtime_error with a readable message on malformed input.
[[nodiscard]] std::shared_ptr<const Lut3D> parseCubeLut(std::string_view text);
[[nodiscard]] std::shared_ptr<const Lut3D> loadCubeLut(const std::string& utf8Path);

// Wipe mask (loki.wipe): the layer is visible left of the moving edge (x < progress, canvas
// coordinates 0..1), with a soft edge. `reverse` keeps the right part instead.
struct WipeMask {
    bool enabled = false;
    float progress = 0.0f;
    float softness = 0.02f;
    bool reverse = false;
};
[[nodiscard]] float wipeAlpha(const WipeMask& wipe, float canvasX) noexcept;

// Per-layer look: color correction, LUT (mixed by `lutMix`), then `dim` towards black (Dip to
// Black). Identical math in the GL shader and the CPU compositor.
struct LayerGrade {
    ColorAdjust color;
    std::shared_ptr<const Lut3D> lut;
    float lutMix = 1.0f;
    float dim = 0.0f; // 0 = unchanged, 1 = black
};
[[nodiscard]] Rgb gradePixel(const LayerGrade& grade, const Rgb& c) noexcept;

} // namespace loki::render
