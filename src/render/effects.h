#pragma once

#include "render/grading.h"
#include "util/rational.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace loki::render {

// --- Masks (D34) ---------------------------------------------------------------------------------

// 8-bit coverage of a layer, stretched over the layer's picture (0 = effect off, 255 = on).
struct MaskImage {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> alpha; // rows top to bottom

    // Bilinear sample at layer coordinates u, v in [0, 1] (texel centers like GL_LINEAR + clamp).
    [[nodiscard]] float sample(float u, float v) const noexcept;
};
using MaskImagePtr = std::shared_ptr<const MaskImage>;

struct MaskPoint {
    double x = 0.0;
    double y = 0.0;
};

// One mask resolved for a time: a closed polygon (bezier flattened) in mask pixels.
struct ResolvedMask {
    std::vector<MaskPoint> polygon;
    double feather = 0.0;   // mask pixels
    double expansion = 0.0; // mask pixels
    float opacity = 1.0f;
    bool subtract = false;
    bool inverted = false;
};

// Coverage of the masks in order: Add = max(acc, a), Subtract = acc * (1 - a); a uses a signed distance
// to the polygon, so expansion grows/shrinks it and feather softens the edge.
[[nodiscard]] MaskImagePtr rasterizeMasks(const std::vector<ResolvedMask>& masks, int width, int height);

// Per-stage masks of a layer (null = the stage applies everywhere).
struct LayerMasks {
    MaskImagePtr opacity;
    MaskImagePtr color;
    MaskImagePtr lumetri;
    MaskImagePtr lut;
    MaskImagePtr blur;

    [[nodiscard]] bool any() const noexcept { return opacity || color || lumetri || lut || blur; }
};

// --- Ultra Key (D35) -----------------------------------------------------------------------------

struct KeyParams {
    bool enabled = false;
    Rgb keyColor{0.0f, 1.0f, 0.0f};
    float tolerance = 0.3f; // chroma distance fully transparent below (0..1)
    float softness = 0.15f; // ramp width above the tolerance
    float choke = 0.0f;     // 0..1 shrinks the matte
    float spill = 0.5f;     // 0..1 removes the key color from edges
    bool matte = false;     // output the alpha as gray (Alpha Channel view)
};
// Applies the key: returns the color (spill removed, or the matte) and multiplies `alpha`.
[[nodiscard]] Rgb keyPixel(const KeyParams& key, const Rgb& c, float& alpha) noexcept;

// --- Gaussian Blur / Sharpen (D37) ----------------------------------------------------------------

// Separable blur taps: integer texel offsets with normalized weights (at most 65 taps; large sigmas
// step over texels). The GPU uses texelFetch at the same offsets, so both agree exactly.
struct BlurTap {
    int offset = 0;
    float weight = 0.0f;
};
[[nodiscard]] std::vector<BlurTap> blurKernel(float sigma);

struct BlurParams {
    float sigma = 0.0f;   // gaussian sigma in decoded-frame pixels (0 = no blur)
    bool horizontal = true;
    bool vertical = true;
    float sharpen = 0.0f; // unsharp amount (0 = none); uses a sigma of 1.5 px when no blur is set

    [[nodiscard]] bool active() const noexcept { return (sigma > 0.0f && (horizontal || vertical)) || sharpen > 0.0f; }
    [[nodiscard]] float kernelSigma() const noexcept { return sigma > 0.0f ? sigma : 1.5f; }
};

// --- Stabilizer (D36) ------------------------------------------------------------------------------

// Frame-to-frame global motion of a picture (source pixels, radians, scale ratio).
struct FrameMotion {
    double dx = 0.0;
    double dy = 0.0;
    double rotation = 0.0;
    double scale = 1.0;
};

using Matrix3 = std::array<double, 9>; // row-major 3x3 affine
[[nodiscard]] Matrix3 identityMatrix() noexcept;
[[nodiscard]] Matrix3 multiply(const Matrix3& a, const Matrix3& b) noexcept;

// Corrections for every analyzed frame: motions[i] is the motion from frame i-1 to frame i (motions[0]
// is ignored). The measured path (cumulative motion) is smoothed with a gaussian whose width follows
// `smoothness` (0..1); each correction moves the content from the measured to the smoothed path,
// around the picture center. `full` also corrects rotation and scale. `autoScale` zooms in just
// enough that no correction uncovers the picture's border (at most 2x).
struct StabilizationPlan {
    std::vector<Matrix3> corrections; // source pixels -> source pixels
    double cropScale = 1.0;
};
// Result of a stabilizer analysis, stored in the effect's `samples` (D36): the motion of every frame
// of the analyzed source range (media pixels), from `start` at `frameRate`.
struct StabilizerAnalysis {
    Rational frameRate;
    Rational start;
    double width = 0.0;  // media size the motions refer to
    double height = 0.0;
    std::vector<FrameMotion> motions;

    // Index of the analyzed frame shown at `sourceTime` (clamped).
    [[nodiscard]] std::size_t indexAt(const Rational& sourceTime) const;
};
[[nodiscard]] std::vector<double> encodeStabilizer(const StabilizerAnalysis& analysis);
[[nodiscard]] std::optional<StabilizerAnalysis> decodeStabilizer(const std::vector<double>& samples);

[[nodiscard]] StabilizationPlan planStabilization(const std::vector<FrameMotion>& motions, double smoothness, bool full,
                                                  double width, double height, bool autoScale);

} // namespace loki::render
