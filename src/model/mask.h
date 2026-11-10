#pragma once

#include "model/ids.h"
#include "model/keyframe.h"
#include "util/rational.h"

#include <string>
#include <vector>

namespace loki::model {

// A vertex of a closed bezier mask path, in media pixels (D34). The handles are offsets from the
// vertex; both zero = a sharp corner.
struct MaskVertex {
    double x = 0.0;
    double y = 0.0;
    double inX = 0.0;
    double inY = 0.0;
    double outX = 0.0;
    double outY = 0.0;

    friend bool operator==(const MaskVertex&, const MaskVertex&) = default;
};

// The path at one source time (animated or tracked masks).
struct MaskPathKey {
    Rational time;
    std::vector<MaskVertex> path;

    friend bool operator==(const MaskPathKey&, const MaskPathKey&) = default;
};

enum class MaskMode { Add = 0, Subtract };

// A mask of one effect (D34): the effect applies inside the path (outside when inverted). Ellipse,
// rectangle and pen masks are all stored as bezier paths.
struct Mask {
    MaskId id;
    std::string name;
    MaskMode mode = MaskMode::Add;
    bool inverted = false;
    std::vector<MaskVertex> path;       // used when `keyframes` is empty
    std::vector<MaskPathKey> keyframes; // sorted by source time, same vertex count
    Parameter feather{"feather", 10.0, {}};    // px of soft edge
    Parameter opacity{"opacity", 100.0, {}};   // %
    Parameter expansion{"expansion", 0.0, {}}; // px, negative shrinks

    // Path at `sourceTime`: linear between path keys, held before the first / after the last.
    [[nodiscard]] std::vector<MaskVertex> pathAt(const Rational& sourceTime) const;

    friend bool operator==(const Mask&, const Mask&) = default;
};

// Problem description (empty = valid).
[[nodiscard]] std::string maskProblem(const Mask& mask);

// Shapes, in media pixels.
[[nodiscard]] std::vector<MaskVertex> ellipsePath(double cx, double cy, double rx, double ry);
[[nodiscard]] std::vector<MaskVertex> rectanglePath(double x, double y, double w, double h);
// Moves (and scales around `pivot`) every vertex and handle.
[[nodiscard]] std::vector<MaskVertex> transformedPath(const std::vector<MaskVertex>& path, double dx, double dy,
                                                      double scale = 1.0, double pivotX = 0.0, double pivotY = 0.0);

// A point of a color curve (D33), both coordinates 0..1.
struct CurvePoint {
    double x = 0.0;
    double y = 0.0;

    friend bool operator==(const CurvePoint&, const CurvePoint&) = default;
};

// Curve problem (empty = valid): at least 2 points, x strictly increasing, all in [0, 1].
[[nodiscard]] std::string curveProblem(const std::vector<CurvePoint>& points);
// The identity curve (0,0)-(1,1).
[[nodiscard]] std::vector<CurvePoint> identityCurve();
// Evaluates a curve with a monotone cubic (Fritsch-Carlson) spline, clamped to [0, 1].
[[nodiscard]] double evaluateCurve(const std::vector<CurvePoint>& points, double x);

} // namespace loki::model
