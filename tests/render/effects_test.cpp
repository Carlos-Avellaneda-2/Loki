// Phase 14 render core: Lumetri (D33), masks (D34), Ultra Key (D35), stabilizer plan (D36),
// blur kernel and irises (D37).
#include "render/cpu_compositor.h"
#include "render/effects.h"
#include "render/grading.h"

#include <gtest/gtest.h>

#include <cmath>
#include <numeric>

namespace {

using namespace loki;
using namespace loki::render;

TEST(LumetriTest, IdentityCurvesAndNeutralWheelsKeepTheColor) {
    const auto table = CurveTable::identity();
    for (const float v : {0.0f, 0.25f, 0.5f, 0.731f, 1.0f}) {
        EXPECT_NEAR(table.lookup(0, v), v, 1e-5f);
    }
    Lumetri lum;
    lum.enabled = true;
    const Rgb c{0.3f, 0.5f, 0.7f};
    const auto out = lumetriPixel(lum, c, 0.5f, 0.5f);
    for (std::size_t i = 0; i < 3; ++i) {
        EXPECT_NEAR(out[i], c[i], 1e-6f);
    }
}

TEST(LumetriTest, WheelOffsetsAreLumaNeutralAndTintTheirRange) {
    const auto warm = wheelOffset(30.0f, 100.0f); // orange
    EXPECT_NEAR(0.2126f * warm[0] + 0.7152f * warm[1] + 0.0722f * warm[2], 0.0f, 1e-5f);
    EXPECT_GT(warm[0], 0.0f);
    EXPECT_LT(warm[2], 0.0f);
    Lumetri lum;
    lum.enabled = true;
    lum.wheelOffset[0] = warm; // shadows
    const auto dark = lumetriPixel(lum, {0.1f, 0.1f, 0.1f}, 0.5f, 0.5f);
    const auto bright = lumetriPixel(lum, {0.9f, 0.9f, 0.9f}, 0.5f, 0.5f);
    EXPECT_GT(dark[0] - dark[2], bright[0] - bright[2]); // shadows get more of the tint
}

TEST(LumetriTest, HslSecondaryChangesOnlyTheSelectedHue) {
    for (const Rgb c : {Rgb{0.2f, 0.7f, 0.3f}, Rgb{0.9f, 0.1f, 0.2f}, Rgb{0.4f, 0.4f, 0.4f}}) {
        const auto back = hslToRgb(rgbToHsl(c));
        for (std::size_t i = 0; i < 3; ++i) {
            EXPECT_NEAR(back[i], c[i], 1e-5f);
        }
    }
    Lumetri lum;
    lum.enabled = true;
    lum.hsl.enabled = true;
    lum.hsl.hueCenter = 120.0f; // greens
    lum.hsl.saturation = 0.0f;  // desaturate them
    const auto green = lumetriPixel(lum, {0.2f, 0.7f, 0.3f}, 0.5f, 0.5f);
    EXPECT_NEAR(green[0], green[1], 1e-4f);
    const Rgb red{0.9f, 0.1f, 0.2f};
    const auto keptRed = lumetriPixel(lum, red, 0.5f, 0.5f);
    EXPECT_NEAR(keptRed[0], red[0], 1e-5f);
    lum.hsl.showMask = true;
    EXPECT_NEAR(lumetriPixel(lum, {0.2f, 0.7f, 0.3f}, 0.5f, 0.5f)[0], 1.0f, 1e-4f);
    EXPECT_NEAR(lumetriPixel(lum, red, 0.5f, 0.5f)[0], 0.0f, 1e-4f);
}

TEST(LumetriTest, VignetteDarkensCornersNotTheCenter) {
    Lumetri lum;
    lum.enabled = true;
    lum.vignette.amount = -100.0f;
    const Rgb c{0.8f, 0.8f, 0.8f};
    EXPECT_NEAR(lumetriPixel(lum, c, 0.5f, 0.5f)[0], 0.8f, 1e-5f);
    EXPECT_LT(lumetriPixel(lum, c, 0.0f, 0.0f)[0], 0.05f);
}

TEST(GradeTest, MasksLimitEachStageAndDipCanGoToWhite) {
    LayerGrade grade;
    grade.color.saturation = 0.0f;
    PixelContext outside;
    outside.colorMask = 0.0f;
    const Rgb c{0.9f, 0.1f, 0.2f};
    const auto kept = gradePixel(grade, c, outside);
    EXPECT_NEAR(kept[0], c[0], 1e-6f);
    const auto gray = gradePixel(grade, c, {});
    EXPECT_NEAR(gray[0], gray[1], 1e-5f);
    LayerGrade dip;
    dip.dim = 1.0f;
    dip.dimTarget = 1.0f;
    EXPECT_NEAR(gradePixel(dip, c)[1], 1.0f, 1e-6f);
}

TEST(MaskTest, RasterizedRectangleHasHardInsideOutsideAndFeatheredEdge) {
    ResolvedMask rect;
    rect.polygon = {{20, 20}, {80, 20}, {80, 80}, {20, 80}};
    auto hard = rasterizeMasks({rect}, 100, 100);
    EXPECT_EQ(hard->alpha[50 * 100 + 50], 255);
    EXPECT_EQ(hard->alpha[5 * 100 + 5], 0);
    rect.feather = 10.0;
    auto soft = rasterizeMasks({rect}, 100, 100);
    EXPECT_NEAR(soft->alpha[50 * 100 + 20], 128, 20); // on the edge: half
    rect.inverted = true;
    EXPECT_EQ(rasterizeMasks({rect}, 100, 100)->alpha[5 * 100 + 5], 255);
    // Subtract a hole in the middle.
    ResolvedMask outer;
    outer.polygon = {{0, 0}, {100, 0}, {100, 100}, {0, 100}};
    ResolvedMask hole;
    hole.polygon = {{40, 40}, {60, 40}, {60, 60}, {40, 60}};
    hole.subtract = true;
    auto both = rasterizeMasks({outer, hole}, 100, 100);
    EXPECT_EQ(both->alpha[50 * 100 + 50], 0);
    EXPECT_EQ(both->alpha[10 * 100 + 10], 255);
    EXPECT_NEAR(both->sample(0.5f, 0.5f), 0.0f, 1e-6f);
}

TEST(KeyTest, KeyColorBecomesTransparentAndOthersStay) {
    KeyParams key;
    key.enabled = true;
    float green = 1.0f;
    (void)keyPixel(key, {0.05f, 0.95f, 0.1f}, green);
    EXPECT_LT(green, 0.05f);
    float skin = 1.0f;
    const auto kept = keyPixel(key, {0.8f, 0.55f, 0.45f}, skin);
    EXPECT_NEAR(skin, 1.0f, 1e-6f);
    EXPECT_NEAR(kept[0], 0.8f, 0.05f);
    // Spill: a greenish edge loses green.
    float edge = 1.0f;
    key.tolerance = 0.0f;
    key.softness = 0.01f;
    const auto despilled = keyPixel(key, {0.5f, 0.7f, 0.5f}, edge);
    EXPECT_LT(despilled[1], 0.7f);
    key.matte = true;
    float matte = 1.0f;
    EXPECT_NEAR(keyPixel(key, {0.8f, 0.55f, 0.45f}, matte)[0], 1.0f, 1e-6f);
}

TEST(BlurTest, KernelIsNormalizedSymmetricAndBounded) {
    for (const float sigma : {0.5f, 3.0f, 40.0f, 200.0f}) {
        const auto taps = blurKernel(sigma);
        EXPECT_LE(taps.size(), 65u);
        const float total = std::accumulate(taps.begin(), taps.end(), 0.0f,
                                            [](float s, const BlurTap& t) { return s + t.weight; });
        EXPECT_NEAR(total, 1.0f, 1e-4f);
        EXPECT_EQ(taps.front().offset, -taps.back().offset);
    }
    EXPECT_EQ(blurKernel(0.0f).size(), 1u);
}

TEST(WipeTest, IrisShapesOpenFromTheCenter) {
    WipeMask iris{true, 0.3f, 0.02f, false, WipeShape::Circle, 16.0f / 9.0f};
    EXPECT_NEAR(wipeAlpha(iris, 0.5f, 0.5f), 1.0f, 1e-5f);
    EXPECT_NEAR(wipeAlpha(iris, 0.0f, 0.0f), 0.0f, 1e-5f);
    iris.progress = 1.0f;
    EXPECT_NEAR(wipeAlpha(iris, 0.0f, 0.0f), 1.0f, 1e-5f);
    WipeMask box{true, 0.5f, 0.02f, false, WipeShape::Box, 1.0f};
    EXPECT_NEAR(wipeAlpha(box, 0.6f, 0.6f), 1.0f, 1e-5f);
    EXPECT_NEAR(wipeAlpha(box, 0.95f, 0.5f), 0.0f, 1e-5f);
}

TEST(StabilizerTest, PlanRemovesJitterAndKeepsSteadyMotion) {
    std::vector<FrameMotion> still(30);
    auto plan = planStabilization(still, 0.5, true, 100, 100, true);
    ASSERT_EQ(plan.corrections.size(), 30u);
    EXPECT_NEAR(plan.cropScale, 1.0, 1e-9);
    EXPECT_NEAR(plan.corrections[10][2], 0.0, 1e-9);
    // Alternating +-2 px shake: corrections cancel most of it and autoscale covers the border.
    std::vector<FrameMotion> shake(60);
    for (std::size_t i = 1; i < shake.size(); ++i) {
        shake[i].dx = (i % 2 == 0) ? 4.0 : -4.0;
    }
    plan = planStabilization(shake, 0.5, false, 100, 100, true);
    EXPECT_GT(plan.cropScale, 1.0);
    EXPECT_LT(plan.cropScale, 1.2);
    // Position after correction of frames 20 and 21 (measured: 0 and -4) almost equal.
    const auto at = [&](std::size_t i, double measured) { return plan.corrections[i][0] * (50.0 + measured) + plan.corrections[i][2]; };
    EXPECT_NEAR(at(20, 0.0), at(21, -4.0), 0.6);
    // Encoding round trip.
    StabilizerAnalysis analysis{Rational(30000, 1001), Rational(2), 1920, 1080, shake};
    const auto decoded = decodeStabilizer(encodeStabilizer(analysis));
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->frameRate, analysis.frameRate);
    EXPECT_EQ(decoded->start, Rational(2));
    EXPECT_EQ(decoded->motions.size(), 60u);
    EXPECT_EQ(decoded->indexAt(Rational(2) + Rational(1001, 3000)), 10u);
    EXPECT_FALSE(decodeStabilizer({1.0, 2.0}).has_value());
}

} // namespace
