#include "model/mask.h"

#include <algorithm>
#include <cmath>

namespace loki::model {

namespace {

bool finite(const MaskVertex& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.inX) && std::isfinite(v.inY) &&
           std::isfinite(v.outX) && std::isfinite(v.outY);
}

std::string pathProblem(const std::vector<MaskVertex>& path) {
    if (path.size() < 3) {
        return "mask path needs at least 3 vertices";
    }
    if (!std::all_of(path.begin(), path.end(), finite)) {
        return "mask path has a non-finite coordinate";
    }
    return {};
}

MaskVertex lerp(const MaskVertex& a, const MaskVertex& b, double t) {
    const auto mix = [t](double x, double y) { return x + (y - x) * t; };
    return {mix(a.x, b.x), mix(a.y, b.y), mix(a.inX, b.inX), mix(a.inY, b.inY), mix(a.outX, b.outX), mix(a.outY, b.outY)};
}

} // namespace

std::vector<MaskVertex> Mask::pathAt(const Rational& sourceTime) const {
    if (keyframes.empty()) {
        return path;
    }
    if (sourceTime <= keyframes.front().time) {
        return keyframes.front().path;
    }
    if (sourceTime >= keyframes.back().time) {
        return keyframes.back().path;
    }
    const auto next = std::upper_bound(keyframes.begin(), keyframes.end(), sourceTime,
                                       [](const Rational& t, const MaskPathKey& k) { return t < k.time; });
    const auto& a = *std::prev(next);
    const auto& b = *next;
    const double t = ((sourceTime - a.time) / (b.time - a.time)).toDouble();
    std::vector<MaskVertex> out(a.path.size());
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = lerp(a.path[i], b.path[i], t);
    }
    return out;
}

std::string maskProblem(const Mask& mask) {
    if (mask.id.isNull()) {
        return "mask has a null id";
    }
    if (mask.keyframes.empty()) {
        if (auto p = pathProblem(mask.path); !p.empty()) {
            return p;
        }
    }
    for (std::size_t i = 0; i < mask.keyframes.size(); ++i) {
        const auto& k = mask.keyframes[i];
        if (auto p = pathProblem(k.path); !p.empty()) {
            return p;
        }
        if (k.path.size() != mask.keyframes.front().path.size()) {
            return "mask path keys have different vertex counts";
        }
        if (i > 0 && !(mask.keyframes[i - 1].time < k.time)) {
            return "mask path keys are not sorted";
        }
    }
    for (const auto* p : {&mask.feather, &mask.opacity, &mask.expansion}) {
        if (!std::isfinite(p->value) || !keyframesValid(p->keyframes)) {
            return "mask parameter '" + p->key + "' is invalid";
        }
    }
    if (mask.mode != MaskMode::Add && mask.mode != MaskMode::Subtract) {
        return "mask has an unknown mode";
    }
    return {};
}

std::vector<MaskVertex> ellipsePath(double cx, double cy, double rx, double ry) {
    // Four bezier arcs: handle length = 0.5523 x radius approximates a circle.
    constexpr double k = 0.5522847498;
    return {
        {cx, cy - ry, -rx * k, 0.0, rx * k, 0.0},
        {cx + rx, cy, 0.0, -ry * k, 0.0, ry * k},
        {cx, cy + ry, rx * k, 0.0, -rx * k, 0.0},
        {cx - rx, cy, 0.0, ry * k, 0.0, -ry * k},
    };
}

std::vector<MaskVertex> rectanglePath(double x, double y, double w, double h) {
    return {{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}};
}

std::vector<MaskVertex> transformedPath(const std::vector<MaskVertex>& path, double dx, double dy, double scale,
                                        double pivotX, double pivotY) {
    std::vector<MaskVertex> out;
    out.reserve(path.size());
    for (const auto& v : path) {
        out.push_back({pivotX + (v.x - pivotX) * scale + dx, pivotY + (v.y - pivotY) * scale + dy, v.inX * scale,
                       v.inY * scale, v.outX * scale, v.outY * scale});
    }
    return out;
}

std::string curveProblem(const std::vector<CurvePoint>& points) {
    if (points.size() < 2) {
        return "curve needs at least 2 points";
    }
    for (std::size_t i = 0; i < points.size(); ++i) {
        const auto& p = points[i];
        if (!(p.x >= 0.0 && p.x <= 1.0 && p.y >= 0.0 && p.y <= 1.0)) {
            return "curve point out of [0, 1]";
        }
        if (i > 0 && !(points[i - 1].x < p.x)) {
            return "curve points are not sorted by x";
        }
    }
    return {};
}

std::vector<CurvePoint> identityCurve() {
    return {{0.0, 0.0}, {1.0, 1.0}};
}

double evaluateCurve(const std::vector<CurvePoint>& p, double x) {
    if (p.size() < 2) {
        return std::clamp(x, 0.0, 1.0);
    }
    if (x <= p.front().x) {
        return std::clamp(p.front().y, 0.0, 1.0);
    }
    if (x >= p.back().x) {
        return std::clamp(p.back().y, 0.0, 1.0);
    }
    const std::size_t n = p.size();
    // Secant slopes, then Fritsch-Carlson tangents (monotone between points).
    std::vector<double> d(n - 1);
    for (std::size_t i = 0; i + 1 < n; ++i) {
        d[i] = (p[i + 1].y - p[i].y) / (p[i + 1].x - p[i].x);
    }
    std::vector<double> m(n);
    m[0] = d[0];
    m[n - 1] = d[n - 2];
    for (std::size_t i = 1; i + 1 < n; ++i) {
        m[i] = d[i - 1] * d[i] <= 0.0 ? 0.0 : (d[i - 1] + d[i]) / 2.0;
    }
    for (std::size_t i = 0; i + 1 < n; ++i) {
        if (d[i] == 0.0) {
            m[i] = 0.0;
            m[i + 1] = 0.0;
            continue;
        }
        const double a = m[i] / d[i];
        const double b = m[i + 1] / d[i];
        const double s = a * a + b * b;
        if (s > 9.0) {
            const double t = 3.0 / std::sqrt(s);
            m[i] = t * a * d[i];
            m[i + 1] = t * b * d[i];
        }
    }
    std::size_t i = 0;
    while (i + 2 < n && x >= p[i + 1].x) {
        ++i;
    }
    const double h = p[i + 1].x - p[i].x;
    const double t = (x - p[i].x) / h;
    const double t2 = t * t;
    const double t3 = t2 * t;
    const double y = (2 * t3 - 3 * t2 + 1) * p[i].y + (t3 - 2 * t2 + t) * h * m[i] + (-2 * t3 + 3 * t2) * p[i + 1].y +
                     (t3 - t2) * h * m[i + 1];
    return std::clamp(y, 0.0, 1.0);
}

} // namespace loki::model
