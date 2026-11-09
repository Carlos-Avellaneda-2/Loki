#include "audio/mixer.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace loki::audio {

float dbToGain(double db) noexcept {
    if (!(db > kSilenceDb)) {
        return 0.0f;
    }
    return static_cast<float>(std::pow(10.0, db / 20.0));
}

double gainToDb(double gain) noexcept {
    if (!(gain > 0.0)) {
        return kSilenceDb;
    }
    return std::max(kSilenceDb, 20.0 * std::log10(gain));
}

StereoGains balanceGains(double pan) noexcept {
    const double p = std::clamp(pan, -100.0, 100.0) / 100.0;
    const auto side = [](double amount) { return static_cast<float>(std::cos(amount * std::numbers::pi / 2.0)); };
    return {p > 0.0 ? side(p) : 1.0f, p < 0.0 ? side(-p) : 1.0f};
}

StereoGains monoPanGains(double pan) noexcept {
    const double p = std::clamp(pan, -100.0, 100.0) / 100.0;
    const double theta = (p + 1.0) * std::numbers::pi / 4.0; // 0 = left, pi/4 = center, pi/2 = right
    // The upmixed input carries 1/sqrt(2) of the mono signal per side: sqrt(2) restores it, and
    // cos/sin keep the total power constant.
    return {static_cast<float>(std::numbers::sqrt2 * std::cos(theta)),
            static_cast<float>(std::numbers::sqrt2 * std::sin(theta))};
}

double fadeGain(double x) noexcept {
    return std::sin(std::clamp(x, 0.0, 1.0) * std::numbers::pi / 2.0);
}

double clipFadeGain(double local, double duration, double fadeIn, double fadeOut) noexcept {
    if (!(duration > 0.0)) {
        return 1.0;
    }
    double in = std::clamp(fadeIn, 0.0, duration);
    double out = std::clamp(fadeOut, 0.0, duration);
    if (in + out > duration) {
        const double scale = duration / (in + out);
        in *= scale;
        out *= scale;
    }
    double g = 1.0;
    if (in > 0.0 && local < in) {
        g *= fadeGain(local / in);
    }
    const double remaining = duration - local;
    if (out > 0.0 && remaining < out) {
        g *= fadeGain(remaining / out);
    }
    return g;
}

namespace {

template <typename Op>
void ramp(std::size_t frames, int channels, StereoGains from, StereoGains to, Op op) noexcept {
    if (frames == 0 || channels <= 0) {
        return;
    }
    const auto ch = static_cast<std::size_t>(channels);
    const float stepL = (to.left - from.left) / static_cast<float>(frames);
    const float stepR = (to.right - from.right) / static_cast<float>(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        const float fi = static_cast<float>(i);
        const float gl = from.left + stepL * fi;
        const float gr = from.right + stepR * fi;
        if (ch == 1) {
            op(i, 0.5f * (gl + gr));
            continue;
        }
        for (std::size_t c = 0; c < ch; ++c) {
            op(i * ch + c, (c % 2 == 0) ? gl : gr);
        }
    }
}

} // namespace

void mixRamp(float* out, const float* in, std::size_t frames, int channels, StereoGains from, StereoGains to) noexcept {
    ramp(frames, channels, from, to, [out, in](std::size_t s, float g) { out[s] += in[s] * g; });
}

void applyRamp(float* buffer, std::size_t frames, int channels, StereoGains from, StereoGains to) noexcept {
    ramp(frames, channels, from, to, [buffer](std::size_t s, float g) { buffer[s] *= g; });
}

StereoLevel StereoLevel::combine(const StereoLevel& a, std::size_t aFrames, const StereoLevel& b,
                                 std::size_t bFrames) noexcept {
    const auto total = static_cast<double>(aFrames + bFrames);
    if (total <= 0.0) {
        return a;
    }
    const auto one = [&](const ChannelLevel& x, const ChannelLevel& y) {
        const double energy = static_cast<double>(x.rms) * x.rms * static_cast<double>(aFrames) +
                              static_cast<double>(y.rms) * y.rms * static_cast<double>(bFrames);
        return ChannelLevel{std::max(x.peak, y.peak), static_cast<float>(std::sqrt(energy / total))};
    };
    return {one(a.left, b.left), one(a.right, b.right)};
}

StereoLevel measure(const float* buffer, std::size_t frames, int channels) noexcept {
    StereoLevel level;
    if (frames == 0 || channels <= 0) {
        return level;
    }
    const auto ch = static_cast<std::size_t>(channels);
    double sumL = 0.0;
    double sumR = 0.0;
    float peakL = 0.0f;
    float peakR = 0.0f;
    for (std::size_t i = 0; i < frames; ++i) {
        const float l = buffer[i * ch];
        const float r = ch > 1 ? buffer[i * ch + 1] : l;
        peakL = std::max(peakL, std::abs(l));
        peakR = std::max(peakR, std::abs(r));
        sumL += static_cast<double>(l) * l;
        sumR += static_cast<double>(r) * r;
    }
    const auto n = static_cast<double>(frames);
    level.left = {peakL, static_cast<float>(std::sqrt(sumL / n))};
    level.right = {peakR, static_cast<float>(std::sqrt(sumR / n))};
    return level;
}

void clampSamples(float* buffer, std::size_t samples) noexcept {
    for (std::size_t i = 0; i < samples; ++i) {
        buffer[i] = std::clamp(buffer[i], -1.0f, 1.0f);
    }
}

} // namespace loki::audio
