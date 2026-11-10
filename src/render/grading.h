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

// Wipe mask (loki.wipe, irises): Linear shows the layer left of the moving edge (x < progress, canvas
// coordinates 0..1), Circle / Box inside a growing centered shape, with a soft edge. `reverse` keeps
// the other part instead. `aspect` = canvas width / height (round irises stay round).
enum class WipeShape { Linear = 0, Circle, Box };
struct WipeMask {
    bool enabled = false;
    float progress = 0.0f;
    float softness = 0.02f;
    bool reverse = false;
    WipeShape shape = WipeShape::Linear;
    float aspect = 16.0f / 9.0f;
};
// Canvas coordinates 0..1, y down.
[[nodiscard]] float wipeAlpha(const WipeMask& wipe, float canvasX, float canvasY = 0.5f) noexcept;

// Curves (D33): per channel output for 256 evenly spaced inputs, master curve already applied.
// Lookups interpolate linearly between entries (as GL_LINEAR on a 256x1 texture does).
struct CurveTable {
    std::array<std::array<float, 256>, 3> rgb{};

    [[nodiscard]] float lookup(std::size_t channel, float v) const noexcept;
    [[nodiscard]] static CurveTable identity() noexcept;
};

// HSL secondary (D33): a hue/saturation/luma qualifier selects pixels; corrections apply there.
struct HslSecondary {
    bool enabled = false;
    float hueCenter = 120.0f; // degrees
    float hueRange = 30.0f;   // +- degrees fully selected
    float softness = 20.0f;   // degrees / percent of falloff
    float satMin = 10.0f;     // % (0..100)
    float lumaMin = 0.0f;     // %
    float lumaMax = 100.0f;   // %
    float hueShift = 0.0f;    // degrees
    float saturation = 100.0f; // % multiplier
    float luma = 0.0f;        // -100..100 brightness offset
    bool showMask = false;    // gray matte of the selection
};

// Vignette (D33), around the layer's center in layer coordinates.
struct Vignette {
    float amount = 0.0f;   // -100 (dark) .. 100 (light)
    float midpoint = 50.0f; // 0..100: where the falloff starts
    float feather = 50.0f;  // 0..100: falloff width
};

// Lumetri Color (D33), applied in this order: color wheels, curves, HSL secondary, vignette.
struct Lumetri {
    bool enabled = false;
    std::array<Rgb, 3> wheelOffset{};  // shadows, midtones, highlights color offsets
    std::array<float, 3> wheelLuma{}; // brightness offsets, -1..1
    std::shared_ptr<const CurveTable> curves; // null = identity
    HslSecondary hsl;
    Vignette vignette;
};
// Color offset of a wheel from its hue (degrees) and amount (0..100): neutral in luma.
[[nodiscard]] Rgb wheelOffset(float hueDegrees, float amount) noexcept;
[[nodiscard]] Rgb lumetriPixel(const Lumetri& lumetri, const Rgb& c, float u, float v) noexcept;
// HSL helpers (h in degrees 0..360, s and l 0..1).
[[nodiscard]] Rgb rgbToHsl(const Rgb& c) noexcept;
[[nodiscard]] Rgb hslToRgb(const Rgb& hsl) noexcept;

// Where a pixel sits in its layer (0..1, y down) and the coverage of the per-stage masks (D34).
struct PixelContext {
    float u = 0.5f;
    float v = 0.5f;
    float colorMask = 1.0f;
    float lumetriMask = 1.0f;
    float lutMask = 1.0f;
};

// Per-layer look: color correction, Lumetri, LUT (mixed by `lutMix`), each mixed by its mask, then
// `dim` towards black or white (dip transitions). Identical math in the GL shader and the CPU
// compositor.
struct LayerGrade {
    ColorAdjust color;
    Lumetri lumetri;
    std::shared_ptr<const Lut3D> lut;
    float lutMix = 1.0f;
    float dim = 0.0f;       // 0 = unchanged, 1 = fully the dim color
    float dimTarget = 0.0f; // 0 = black (Dip to Black), 1 = white (Dip to White)
};
[[nodiscard]] Rgb gradePixel(const LayerGrade& grade, const Rgb& c, const PixelContext& ctx = {}) noexcept;

} // namespace loki::render
