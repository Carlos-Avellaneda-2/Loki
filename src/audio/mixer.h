#pragma once

#include <cstddef>

namespace loki::audio {

// Pure mixing math shared by preview and export (no state, no allocation). Buffers are
// interleaved float frames; channel 0 is left and channel 1 right (other channels follow the
// left/right gain of their parity).

// Levels at or below this many dB are silence.
inline constexpr double kSilenceDb = -96.0;

[[nodiscard]] float dbToGain(double db) noexcept;
// Linear gain to dB, floored at kSilenceDb.
[[nodiscard]] double gainToDb(double gain) noexcept;

struct StereoGains {
    float left = 1.0f;
    float right = 1.0f;

    [[nodiscard]] StereoGains operator*(float g) const noexcept { return {left * g, right * g}; }
    [[nodiscard]] StereoGains operator*(StereoGains o) const noexcept { return {left * o.left, right * o.right}; }
    friend bool operator==(const StereoGains&, const StereoGains&) = default;
};

// Balance for stereo material (and for track/master buses): the center is unity; moving to one
// side fades the other one out with a quarter-cosine. `pan` in [-100, 100].
[[nodiscard]] StereoGains balanceGains(double pan) noexcept;

// Constant-power pan for mono material that was upmixed to both channels at -3 dB: the center
// keeps that -3 dB per side and a hard pan sends the full mono signal to one side.
[[nodiscard]] StereoGains monoPanGains(double pan) noexcept;

// Equal-power fade curve: x in [0, 1] -> sin(x * pi / 2). Values outside are clamped.
[[nodiscard]] double fadeGain(double x) noexcept;

// Gain of the D17 fade handles at `local` seconds into a clip of `duration` seconds. Fades longer
// than the clip are clamped; overlapping fades are scaled to share the clip.
[[nodiscard]] double clipFadeGain(double local, double duration, double fadeIn, double fadeOut) noexcept;

// out += in * gain, the gain moving linearly from `from` (first frame) to `to` (after the last).
void mixRamp(float* out, const float* in, std::size_t frames, int channels, StereoGains from, StereoGains to) noexcept;
// buffer *= gain with the same ramp.
void applyRamp(float* buffer, std::size_t frames, int channels, StereoGains from, StereoGains to) noexcept;

struct ChannelLevel {
    float peak = 0.0f; // max |sample|
    float rms = 0.0f;

    friend bool operator==(const ChannelLevel&, const ChannelLevel&) = default;
};

struct StereoLevel {
    ChannelLevel left;
    ChannelLevel right;

    // Combines two consecutive measurements of `aFrames` and `bFrames` frames.
    [[nodiscard]] static StereoLevel combine(const StereoLevel& a, std::size_t aFrames, const StereoLevel& b,
                                             std::size_t bFrames) noexcept;
    friend bool operator==(const StereoLevel&, const StereoLevel&) = default;
};

// Peak and RMS per side of a block (mono buffers report the same level on both sides).
[[nodiscard]] StereoLevel measure(const float* buffer, std::size_t frames, int channels) noexcept;

// Hard limit to [-1, 1] (the final output stage).
void clampSamples(float* buffer, std::size_t samples) noexcept;

} // namespace loki::audio
