// Phase 15 audio effects (D38): EQ, compressor, noise reduction, FFT and pitch-keeping stretch.
#include "audio/effects.h"
#include "audio/mixer.h"

#include <gtest/gtest.h>

#include <cmath>
#include <numbers>
#include <random>

namespace {

using namespace loki::audio;

constexpr int kRate = 48000;

std::vector<float> sine(double freq, std::size_t frames, int channels, float amplitude = 0.5f) {
    std::vector<float> out(frames * static_cast<std::size_t>(channels));
    for (std::size_t i = 0; i < frames; ++i) {
        const auto v = static_cast<float>(amplitude * std::sin(2.0 * std::numbers::pi * freq * static_cast<double>(i) / kRate));
        for (int c = 0; c < channels; ++c) {
            out[i * static_cast<std::size_t>(channels) + static_cast<std::size_t>(c)] = v;
        }
    }
    return out;
}

double rms(const float* data, std::size_t samples) {
    double sum = 0.0;
    for (std::size_t i = 0; i < samples; ++i) {
        sum += static_cast<double>(data[i]) * data[i];
    }
    return std::sqrt(sum / static_cast<double>(std::max<std::size_t>(samples, 1)));
}

double crossingsPerSecond(const std::vector<float>& buf, std::size_t from, std::size_t frames, int channels) {
    int n = 0;
    for (std::size_t i = from + 1; i < from + frames; ++i) {
        if ((buf[(i - 1) * static_cast<std::size_t>(channels)] < 0.0f) != (buf[i * static_cast<std::size_t>(channels)] < 0.0f)) {
            ++n;
        }
    }
    return n * static_cast<double>(kRate) / static_cast<double>(frames);
}

TEST(EqTest, BandsBoostAndCutTheirFrequencies) {
    Biquad low;
    low.design(Biquad::Kind::LowShelf, kRate, 100, 12, 0.707);
    EXPECT_NEAR(20 * std::log10(low.magnitudeAt(20, kRate)), 12.0, 0.6);
    EXPECT_NEAR(20 * std::log10(low.magnitudeAt(10000, kRate)), 0.0, 0.3);
    Biquad peak;
    peak.design(Biquad::Kind::Peak, kRate, 1000, -9, 1.0);
    EXPECT_NEAR(20 * std::log10(peak.magnitudeAt(1000, kRate)), -9.0, 0.1);
    ParametricEq eq;
    EqParams p;
    p.highGainDb = -12;
    eq.setParams(p, kRate);
    auto hiss = sine(15000, 4800, 2);
    eq.process(hiss.data(), 4800, 2);
    EXPECT_LT(rms(hiss.data() + 2000, 2000), 0.5 / std::sqrt(2.0) * 0.4); // about -12 dB
}

TEST(CompressorTest, ReducesLevelsAboveTheThreshold) {
    Compressor c;
    CompressorParams p;
    p.thresholdDb = -20;
    p.ratio = 4;
    c.setParams(p, kRate);
    EXPECT_FLOAT_EQ(c.gainReductionDb(-30), 0.0f);
    EXPECT_NEAR(c.gainReductionDb(-8), 9.0f, 1e-4f); // 12 dB over / 4 -> 3 dB left -> 9 dB less
    auto loud = sine(440, 48000, 2, 0.9f);
    c.process(loud.data(), 48000, 2);
    float peak = 0.0f;
    for (std::size_t i = 24000 * 2; i < loud.size(); ++i) {
        peak = std::max(peak, std::abs(loud[i]));
    }
    EXPECT_LT(peak, 0.35f); // -0.9 dB in, about -15.7 dB out
}

TEST(FftTest, ForwardThenInverseIsIdentity) {
    std::vector<std::complex<float>> data(256);
    for (std::size_t i = 0; i < data.size(); ++i) {
        data[i] = {static_cast<float>(std::sin(0.1 * i)), 0.0f};
    }
    const auto original = data;
    fft(data, false);
    fft(data, true);
    for (std::size_t i = 0; i < data.size(); ++i) {
        EXPECT_NEAR(data[i].real(), original[i].real(), 1e-4f);
    }
}

TEST(NoiseReducerTest, LowersSteadyNoiseAndKeepsTheSignalAligned) {
    std::mt19937 rng(5);
    std::normal_distribution<float> noise(0.0f, 0.05f);
    const std::size_t frames = 4 * kRate;
    auto noisy = sine(440, frames, 1, 0.0f);
    for (std::size_t i = 0; i < frames; ++i) {
        noisy[i] = noise(rng) + (i >= 2 * kRate ? 0.5f * static_cast<float>(std::sin(2.0 * std::numbers::pi * 440.0 * i / kRate)) : 0.0f);
    }
    NoiseReducer nr;
    nr.setParams(20.0f, 1);
    auto out = noisy;
    for (std::size_t i = 0; i < frames; i += 512) { // device-sized blocks
        nr.process(out.data() + i, std::min<std::size_t>(512, frames - i), 1);
    }
    const auto L = NoiseReducer::latency();
    // Noise-only second: at least 7 dB quieter (spectral subtraction on white noise).
    EXPECT_LT(rms(out.data() + kRate + L, kRate / 2), rms(noisy.data() + kRate, kRate / 2) * 0.45);
    // The tone survives (delayed by the latency).
    EXPECT_GT(rms(out.data() + 3 * kRate + L / 2, kRate / 2), 0.3);
}

TEST(TimeStretcherTest, DoubleSpeedKeepsThePitch) {
    const auto source = sine(440, 4 * kRate, 2);
    TimeStretcher s;
    const std::size_t outFrames = kRate; // 1 s out = 2 s of source
    std::vector<float> out(outFrames * 2);
    const auto fetch = [&](std::int64_t pos, std::size_t frames, float* dst) {
        for (std::size_t i = 0; i < frames; ++i) {
            const auto f = pos + static_cast<std::int64_t>(i);
            for (std::size_t c = 0; c < 2; ++c) {
                dst[i * 2 + c] = f >= 0 && f < static_cast<std::int64_t>(source.size() / 2) ? source[static_cast<std::size_t>(f) * 2 + c] : 0.0f;
            }
        }
    };
    for (std::size_t i = 0; i < outFrames; i += 480) {
        s.render(static_cast<std::int64_t>(i), 480, 2, [](std::int64_t f) { return 2.0 * static_cast<double>(f); }, fetch,
                 out.data() + i * 2);
    }
    EXPECT_NEAR(crossingsPerSecond(out, 4800, kRate / 2, 2), 880.0, 40.0); // 440 Hz, not 880 Hz
    EXPECT_GT(rms(out.data() + 9600, kRate), 0.25);
}

} // namespace
