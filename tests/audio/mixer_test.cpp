#include "audio/mixer.h"

#include <gtest/gtest.h>

#include <cmath>
#include <numbers>
#include <vector>

namespace {

using namespace loki::audio;

TEST(MixerMathTest, DecibelConversions) {
    EXPECT_FLOAT_EQ(dbToGain(0.0), 1.0f);
    EXPECT_NEAR(dbToGain(-6.0206), 0.5f, 1e-4f);
    EXPECT_NEAR(dbToGain(6.0206), 2.0f, 1e-3f);
    EXPECT_EQ(dbToGain(kSilenceDb), 0.0f);
    EXPECT_EQ(dbToGain(-200.0), 0.0f);
    EXPECT_NEAR(gainToDb(0.5), -6.0206, 1e-3);
    EXPECT_EQ(gainToDb(0.0), kSilenceDb);
}

TEST(MixerMathTest, BalanceKeepsUnityAtCenter) {
    EXPECT_EQ(balanceGains(0), (StereoGains{1.0f, 1.0f}));
    const auto right = balanceGains(100);
    EXPECT_NEAR(right.left, 0.0f, 1e-6f);
    EXPECT_EQ(right.right, 1.0f);
    const auto half = balanceGains(-50);
    EXPECT_EQ(half.left, 1.0f);
    EXPECT_NEAR(half.right, std::cos(std::numbers::pi / 4), 1e-6);
    EXPECT_EQ(balanceGains(500), balanceGains(100)); // clamped
}

TEST(MixerMathTest, MonoPanIsConstantPower) {
    for (const double pan : {-100.0, -60.0, 0.0, 25.0, 100.0}) {
        const auto g = monoPanGains(pan);
        // Input per side is m/sqrt(2): output power (gl^2 + gr^2) / 2 stays 1.
        EXPECT_NEAR((g.left * g.left + g.right * g.right) / 2.0, 1.0, 1e-5) << pan;
    }
    EXPECT_NEAR(monoPanGains(0).left, 1.0f, 1e-6f); // center: the upmixed -3 dB unchanged
    EXPECT_NEAR(monoPanGains(-100).right, 0.0f, 1e-6f);
}

TEST(MixerMathTest, FadeCurves) {
    EXPECT_EQ(fadeGain(0.0), 0.0);
    EXPECT_NEAR(fadeGain(0.5), std::sqrt(0.5), 1e-9);
    EXPECT_EQ(fadeGain(1.0), 1.0);
    EXPECT_EQ(fadeGain(3.0), 1.0);
    // 4 s clip, 1 s fade in, 2 s fade out.
    EXPECT_EQ(clipFadeGain(0.0, 4.0, 1.0, 2.0), 0.0);
    EXPECT_NEAR(clipFadeGain(0.5, 4.0, 1.0, 2.0), std::sqrt(0.5), 1e-9);
    EXPECT_EQ(clipFadeGain(1.5, 4.0, 1.0, 2.0), 1.0);
    EXPECT_NEAR(clipFadeGain(3.0, 4.0, 1.0, 2.0), std::sqrt(0.5), 1e-9);
    // Fades longer than the clip share it proportionally (here 1:1 over 2 s).
    EXPECT_EQ(clipFadeGain(1.0, 2.0, 5.0, 5.0), 1.0);
    EXPECT_NEAR(clipFadeGain(0.5, 2.0, 5.0, 5.0), std::sqrt(0.5), 1e-9);
}

TEST(MixerMathTest, RampsAreLinearPerChannel) {
    std::vector<float> in(4 * 2, 1.0f);
    std::vector<float> out(4 * 2, 0.5f);
    mixRamp(out.data(), in.data(), 4, 2, {0.0f, 1.0f}, {1.0f, 0.0f});
    EXPECT_FLOAT_EQ(out[0], 0.5f);  // L frame 0: gain 0
    EXPECT_FLOAT_EQ(out[1], 1.5f);  // R frame 0: gain 1
    EXPECT_FLOAT_EQ(out[6], 1.25f); // L frame 3: gain 0.75
    EXPECT_FLOAT_EQ(out[7], 0.75f); // R frame 3: gain 0.25
    applyRamp(out.data(), 4, 2, {2.0f, 2.0f}, {2.0f, 2.0f});
    EXPECT_FLOAT_EQ(out[0], 1.0f);
}

TEST(MixerMathTest, MeasuresPeakAndRms) {
    const std::size_t n = 4800;
    std::vector<float> sine(n * 2);
    for (std::size_t i = 0; i < n; ++i) {
        const auto v = static_cast<float>(0.5 * std::sin(2.0 * std::numbers::pi * 440.0 * static_cast<double>(i) / 48000.0));
        sine[i * 2] = v;
        sine[i * 2 + 1] = -0.25f * v / 0.5f;
    }
    const auto level = measure(sine.data(), n, 2);
    EXPECT_NEAR(level.left.peak, 0.5f, 1e-3f);
    EXPECT_NEAR(level.left.rms, 0.5f / std::sqrt(2.0f), 1e-3f);
    EXPECT_NEAR(level.right.peak, 0.25f, 1e-3f);
    // Combining a silent block of equal length halves the energy.
    const auto both = StereoLevel::combine(level, n, StereoLevel{}, n);
    EXPECT_EQ(both.left.peak, level.left.peak);
    EXPECT_NEAR(both.left.rms, level.left.rms / std::sqrt(2.0f), 1e-4f);
    std::vector<float> loud = {1.5f, -2.0f, 0.2f, 0.1f};
    clampSamples(loud.data(), loud.size());
    EXPECT_EQ(loud, (std::vector<float>{1.0f, -1.0f, 0.2f, 0.1f}));
}

} // namespace
