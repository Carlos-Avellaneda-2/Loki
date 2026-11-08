#pragma once

#include "util/rational.h"

#include <string>
#include <vector>

namespace loki::model {

// Interpolation of the segment that starts at a keyframe and ends at the next one.
enum class Interpolation {
    Hold,   // value jumps at the next keyframe
    Linear, // straight line
    Bezier, // cubic ease: zero slope at both ends, shaped by the influences
};

struct Keyframe {
    // Time in the clip's *source* timeline, so keyframes follow the content when the clip is
    // moved, trimmed or split.
    Rational time;
    double value = 0.0;
    Interpolation interpolation = Interpolation::Linear;
    // Bezier ease influence in [0, 1]: fraction of the segment the handle extends into.
    double easeOut = 1.0 / 3.0; // handle leaving this keyframe
    double easeIn = 1.0 / 3.0;  // handle arriving at this keyframe

    friend bool operator==(const Keyframe&, const Keyframe&) = default;
};

// An effect parameter: either a static value or a keyframed curve.
struct Parameter {
    std::string key; // stable identifier within its effect, e.g. "scale"
    double value = 0.0;            // used when keyframes is empty
    std::vector<Keyframe> keyframes; // sorted by time, unique times

    [[nodiscard]] bool isAnimated() const noexcept { return !keyframes.empty(); }
    // Value at `sourceTime`. Before the first / after the last keyframe the curve is flat.
    [[nodiscard]] double valueAt(const Rational& sourceTime) const;

    friend bool operator==(const Parameter&, const Parameter&) = default;
};

// Evaluates one segment between `a` and `b` (a.time < b.time) at `t` in [a.time, b.time].
[[nodiscard]] double interpolateSegment(const Keyframe& a, const Keyframe& b, const Rational& t);

// True if keyframes are strictly increasing in time and influences are within [0, 1].
[[nodiscard]] bool keyframesValid(const std::vector<Keyframe>& keyframes) noexcept;

} // namespace loki::model
