#include "model/keyframe.h"

#include <algorithm>
#include <cmath>

namespace loki::model {

namespace {

double cubic(double p0, double p1, double p2, double p3, double s) noexcept {
    const double u = 1.0 - s;
    return u * u * u * p0 + 3.0 * u * u * s * p1 + 3.0 * u * s * s * p2 + s * s * s * p3;
}

double clamp01(double v) noexcept {
    return std::clamp(v, 0.0, 1.0);
}

// Cubic bezier on the unit square: x = time fraction, y = value fraction.
// Control points (0,0), (out,0), (1-in,1), (1,1) give an ease with flat tangents at both ends.
double easeCurve(double x, double out, double in) noexcept {
    const double x1 = clamp01(out);
    const double x2 = 1.0 - clamp01(in);
    // x(s) is monotonic on [0,1] because 0 <= x1, x2 <= 1: bisection always converges.
    double lo = 0.0;
    double hi = 1.0;
    double s = x;
    for (int i = 0; i < 64; ++i) {
        s = 0.5 * (lo + hi);
        const double xs = cubic(0.0, x1, x2, 1.0, s);
        if (std::abs(xs - x) < 1e-12) {
            break;
        }
        (xs < x ? lo : hi) = s;
    }
    return cubic(0.0, 0.0, 1.0, 1.0, s);
}

} // namespace

double interpolateSegment(const Keyframe& a, const Keyframe& b, const Rational& t) {
    if (t <= a.time) {
        return a.value;
    }
    if (t >= b.time) {
        return b.value;
    }
    // Fraction computed exactly, converted to double only for the value blend.
    const double x = ((t - a.time) / (b.time - a.time)).toDouble();
    switch (a.interpolation) {
    case Interpolation::Hold: return a.value;
    case Interpolation::Linear: return a.value + (b.value - a.value) * x;
    case Interpolation::Bezier: return a.value + (b.value - a.value) * easeCurve(x, a.easeOut, b.easeIn);
    }
    return a.value;
}

double Parameter::valueAt(const Rational& sourceTime) const {
    if (keyframes.empty()) {
        return value;
    }
    if (sourceTime <= keyframes.front().time) {
        return keyframes.front().value;
    }
    if (sourceTime >= keyframes.back().time) {
        return keyframes.back().value;
    }
    const auto next = std::upper_bound(keyframes.begin(), keyframes.end(), sourceTime,
                                       [](const Rational& t, const Keyframe& k) { return t < k.time; });
    const auto prev = std::prev(next);
    return interpolateSegment(*prev, *next, sourceTime);
}

bool keyframesValid(const std::vector<Keyframe>& keyframes) noexcept {
    for (std::size_t i = 0; i < keyframes.size(); ++i) {
        const auto& k = keyframes[i];
        if (!(k.easeIn >= 0.0 && k.easeIn <= 1.0 && k.easeOut >= 0.0 && k.easeOut <= 1.0)) {
            return false;
        }
        if (!std::isfinite(k.value)) {
            return false;
        }
        if (i > 0 && !(keyframes[i - 1].time < k.time)) {
            return false;
        }
    }
    return true;
}

} // namespace loki::model
