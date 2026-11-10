#include "render/effects.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace loki::render {

namespace {

float clamp01(float v) noexcept {
    return std::clamp(v, 0.0f, 1.0f);
}

float smoothstep(float e0, float e1, float x) noexcept {
    const float t = clamp01((x - e0) / (e1 - e0));
    return t * t * (3.0f - 2.0f * t);
}

float luma709(const Rgb& c) noexcept {
    return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
}

// Signed distance from (x, y) to a closed polygon: positive inside (even-odd rule).
double signedDistance(const std::vector<MaskPoint>& poly, double x, double y) {
    double best = std::numeric_limits<double>::max();
    bool inside = false;
    const std::size_t n = poly.size();
    for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
        const auto& a = poly[j];
        const auto& b = poly[i];
        if ((b.y > y) != (a.y > y) && x < (a.x - b.x) * (y - b.y) / (a.y - b.y) + b.x) {
            inside = !inside;
        }
        const double ex = b.x - a.x;
        const double ey = b.y - a.y;
        const double len2 = ex * ex + ey * ey;
        double t = len2 > 0.0 ? ((x - a.x) * ex + (y - a.y) * ey) / len2 : 0.0;
        t = std::clamp(t, 0.0, 1.0);
        const double dx = a.x + ex * t - x;
        const double dy = a.y + ey * t - y;
        best = std::min(best, dx * dx + dy * dy);
    }
    const double d = std::sqrt(best);
    return inside ? d : -d;
}

} // namespace

float MaskImage::sample(float u, float v) const noexcept {
    if (width <= 0 || height <= 0 || alpha.empty()) {
        return 1.0f;
    }
    const float x = std::clamp(u * static_cast<float>(width) - 0.5f, 0.0f, static_cast<float>(width - 1));
    const float y = std::clamp(v * static_cast<float>(height) - 0.5f, 0.0f, static_cast<float>(height - 1));
    const int x0 = static_cast<int>(x);
    const int y0 = static_cast<int>(y);
    const int x1 = std::min(x0 + 1, width - 1);
    const int y1 = std::min(y0 + 1, height - 1);
    const float fx = x - static_cast<float>(x0);
    const float fy = y - static_cast<float>(y0);
    const auto at = [&](int px, int py) {
        return alpha[static_cast<std::size_t>(py) * static_cast<std::size_t>(width) + static_cast<std::size_t>(px)] / 255.0f;
    };
    const float top = at(x0, y0) + (at(x1, y0) - at(x0, y0)) * fx;
    const float bottom = at(x0, y1) + (at(x1, y1) - at(x0, y1)) * fx;
    return top + (bottom - top) * fy;
}

MaskImagePtr rasterizeMasks(const std::vector<ResolvedMask>& masks, int width, int height) {
    auto image = std::make_shared<MaskImage>();
    image->width = std::max(1, width);
    image->height = std::max(1, height);
    std::vector<float> acc(static_cast<std::size_t>(image->width) * static_cast<std::size_t>(image->height), 0.0f);
    for (const auto& mask : masks) {
        if (mask.polygon.size() < 3) {
            continue;
        }
        double minX = mask.polygon.front().x;
        double maxX = minX;
        double minY = mask.polygon.front().y;
        double maxY = minY;
        for (const auto& p : mask.polygon) {
            minX = std::min(minX, p.x);
            maxX = std::max(maxX, p.x);
            minY = std::min(minY, p.y);
            maxY = std::max(maxY, p.y);
        }
        const double reach = std::abs(mask.feather) + std::abs(mask.expansion) + 1.0;
        for (int y = 0; y < image->height; ++y) {
            const double py = y + 0.5;
            for (int x = 0; x < image->width; ++x) {
                const double px = x + 0.5;
                float a = 0.0f;
                if (px >= minX - reach && px <= maxX + reach && py >= minY - reach && py <= maxY + reach) {
                    const double d = signedDistance(mask.polygon, px, py) + mask.expansion;
                    a = mask.feather > 0.0 ? clamp01(static_cast<float>(d / mask.feather + 0.5)) : (d >= 0.0 ? 1.0f : 0.0f);
                }
                if (mask.inverted) {
                    a = 1.0f - a;
                }
                a *= clamp01(mask.opacity);
                auto& v = acc[static_cast<std::size_t>(y) * static_cast<std::size_t>(image->width) + static_cast<std::size_t>(x)];
                v = mask.subtract ? v * (1.0f - a) : std::max(v, a);
            }
        }
    }
    image->alpha.resize(acc.size());
    for (std::size_t i = 0; i < acc.size(); ++i) {
        image->alpha[i] = static_cast<std::uint8_t>(std::lround(clamp01(acc[i]) * 255.0f));
    }
    return image;
}

Rgb keyPixel(const KeyParams& key, const Rgb& in, float& alpha) noexcept {
    if (!key.enabled) {
        return in;
    }
    const float y = luma709(in);
    const float cb = (in[2] - y) / 1.8556f;
    const float cr = (in[0] - y) / 1.5748f;
    const float ky = luma709(key.keyColor);
    const float kcb = (key.keyColor[2] - ky) / 1.8556f;
    const float kcr = (key.keyColor[0] - ky) / 1.5748f;
    const float dist = std::sqrt((cb - kcb) * (cb - kcb) + (cr - kcr) * (cr - kcr)) * 2.0f;
    float a = smoothstep(key.tolerance, key.tolerance + std::max(key.softness, 0.001f), dist);
    a = clamp01((a - key.choke) / std::max(1.0f - key.choke, 0.001f));
    if (key.matte) {
        return {a, a, a};
    }
    Rgb c = in;
    const Rgb dir{key.keyColor[0] - ky, key.keyColor[1] - ky, key.keyColor[2] - ky};
    const float len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    if (len > 1e-4f && key.spill > 0.0f) {
        const float amount = std::max(0.0f, ((c[0] - y) * dir[0] + (c[1] - y) * dir[1] + (c[2] - y) * dir[2]) / len);
        for (std::size_t i = 0; i < 3; ++i) {
            c[i] = clamp01(c[i] - dir[i] / len * amount * key.spill);
        }
    }
    alpha *= a;
    return c;
}

std::vector<BlurTap> blurKernel(float sigma) {
    std::vector<BlurTap> taps;
    if (!(sigma > 0.0f)) {
        taps.push_back({0, 1.0f});
        return taps;
    }
    constexpr int kHalfTaps = 32;
    const int radius = std::max(1, static_cast<int>(std::ceil(3.0f * sigma)));
    const int step = std::max(1, (radius + kHalfTaps - 1) / kHalfTaps);
    float total = 0.0f;
    for (int k = -radius / step; k <= radius / step; ++k) {
        const int offset = k * step;
        const float w = std::exp(-static_cast<float>(offset * offset) / (2.0f * sigma * sigma));
        taps.push_back({offset, w});
        total += w;
    }
    for (auto& t : taps) {
        t.weight /= total;
    }
    return taps;
}

Matrix3 identityMatrix() noexcept {
    return {1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
}

Matrix3 multiply(const Matrix3& a, const Matrix3& b) noexcept {
    Matrix3 r{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            double s = 0.0;
            for (int k = 0; k < 3; ++k) {
                s += a[static_cast<std::size_t>(i * 3 + k)] * b[static_cast<std::size_t>(k * 3 + j)];
            }
            r[static_cast<std::size_t>(i * 3 + j)] = s;
        }
    }
    return r;
}

namespace {

Matrix3 translation(double x, double y) noexcept {
    return {1.0, 0.0, x, 0.0, 1.0, y, 0.0, 0.0, 1.0};
}

// Rotation (clockwise, y down) and uniform scale around (cx, cy).
Matrix3 rotateScaleAround(double angle, double scale, double cx, double cy) noexcept {
    const double c = std::cos(angle) * scale;
    const double s = std::sin(angle) * scale;
    const Matrix3 rs{c, -s, 0.0, s, c, 0.0, 0.0, 0.0, 1.0};
    return multiply(translation(cx, cy), multiply(rs, translation(-cx, -cy)));
}

std::array<double, 2> apply(const Matrix3& m, double x, double y) noexcept {
    return {m[0] * x + m[1] * y + m[2], m[3] * x + m[4] * y + m[5]};
}

bool invert(const Matrix3& m, Matrix3& out) noexcept {
    const double det = m[0] * m[4] - m[1] * m[3];
    if (std::abs(det) < 1e-12) {
        return false;
    }
    const double ia = m[4] / det;
    const double ib = -m[1] / det;
    const double id = -m[3] / det;
    const double ie = m[0] / det;
    out = {ia, ib, -(ia * m[2] + ib * m[5]), id, ie, -(id * m[2] + ie * m[5]), 0.0, 0.0, 1.0};
    return true;
}

// True when every corner of the picture shows source pixels after `m` (source -> shown).
bool covers(const Matrix3& m, double w, double h) noexcept {
    Matrix3 inv;
    if (!invert(m, inv)) {
        return false;
    }
    for (const auto& [x, y] : {std::pair{0.0, 0.0}, std::pair{w, 0.0}, std::pair{0.0, h}, std::pair{w, h}}) {
        const auto p = apply(inv, x, y);
        if (p[0] < -1e-6 || p[1] < -1e-6 || p[0] > w + 1e-6 || p[1] > h + 1e-6) {
            return false;
        }
    }
    return true;
}

} // namespace

StabilizationPlan planStabilization(const std::vector<FrameMotion>& motions, double smoothness, bool full, double width,
                                    double height, bool autoScale) {
    StabilizationPlan plan;
    const std::size_t n = motions.size();
    if (n == 0) {
        return plan;
    }
    // Measured (cumulative) path.
    std::vector<std::array<double, 4>> path(n); // x, y, angle, log scale
    for (std::size_t i = 1; i < n; ++i) {
        const auto& m = motions[i];
        path[i] = {path[i - 1][0] + m.dx, path[i - 1][1] + m.dy, path[i - 1][2] + m.rotation,
                   path[i - 1][3] + std::log(std::max(m.scale, 1e-6))};
    }
    // Gaussian smoothing; the window is renormalized at the ends.
    const double sigma = std::clamp(smoothness, 0.0, 1.0) * 30.0;
    std::vector<std::array<double, 4>> smooth = path;
    if (sigma > 0.01) {
        const auto radius = static_cast<std::ptrdiff_t>(std::ceil(3.0 * sigma));
        for (std::size_t i = 0; i < n; ++i) {
            std::array<double, 4> sum{};
            double total = 0.0;
            for (std::ptrdiff_t k = -radius; k <= radius; ++k) {
                const auto j = static_cast<std::ptrdiff_t>(i) + k;
                if (j < 0 || j >= static_cast<std::ptrdiff_t>(n)) {
                    continue;
                }
                const double w = std::exp(-static_cast<double>(k * k) / (2.0 * sigma * sigma));
                for (std::size_t c = 0; c < 4; ++c) {
                    sum[c] += path[static_cast<std::size_t>(j)][c] * w;
                }
                total += w;
            }
            for (std::size_t c = 0; c < 4; ++c) {
                smooth[i][c] = sum[c] / total;
            }
        }
    }
    const double cx = width / 2.0;
    const double cy = height / 2.0;
    plan.corrections.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double dx = smooth[i][0] - path[i][0];
        const double dy = smooth[i][1] - path[i][1];
        const double angle = full ? smooth[i][2] - path[i][2] : 0.0;
        const double scale = full ? std::exp(smooth[i][3] - path[i][3]) : 1.0;
        plan.corrections[i] = multiply(translation(dx, dy), rotateScaleAround(angle, scale, cx, cy));
    }
    if (autoScale) {
        double needed = 1.0;
        for (const auto& c : plan.corrections) {
            double lo = 1.0;
            double hi = 2.0;
            if (covers(c, width, height)) {
                continue;
            }
            for (int it = 0; it < 30; ++it) {
                const double mid = (lo + hi) / 2.0;
                (covers(multiply(rotateScaleAround(0.0, mid, cx, cy), c), width, height) ? hi : lo) = mid;
            }
            needed = std::max(needed, hi);
        }
        plan.cropScale = needed;
        const auto zoom = rotateScaleAround(0.0, needed, cx, cy);
        for (auto& c : plan.corrections) {
            c = multiply(zoom, c);
        }
    }
    return plan;
}

std::size_t StabilizerAnalysis::indexAt(const Rational& sourceTime) const {
    if (motions.empty() || !frameRate.isPositive()) {
        return 0;
    }
    const auto i = timeToFrames(max(Rational(), sourceTime - start), frameRate, Rounding::Floor);
    return static_cast<std::size_t>(std::clamp<std::int64_t>(i, 0, static_cast<std::int64_t>(motions.size()) - 1));
}

namespace {
constexpr double kStabilizerFormat = 1.0;
constexpr std::size_t kStabilizerHeader = 8;
} // namespace

std::vector<double> encodeStabilizer(const StabilizerAnalysis& a) {
    std::vector<double> s{kStabilizerFormat,
                          static_cast<double>(a.frameRate.num()),
                          static_cast<double>(a.frameRate.den()),
                          static_cast<double>(a.start.num()),
                          static_cast<double>(a.start.den()),
                          a.width,
                          a.height,
                          static_cast<double>(a.motions.size())};
    for (const auto& m : a.motions) {
        s.insert(s.end(), {m.dx, m.dy, m.rotation, m.scale});
    }
    return s;
}

std::optional<StabilizerAnalysis> decodeStabilizer(const std::vector<double>& s) {
    if (s.size() < kStabilizerHeader || s[0] != kStabilizerFormat) {
        return std::nullopt;
    }
    const auto count = static_cast<std::size_t>(s[7]);
    if (s[7] < 0.0 || s.size() != kStabilizerHeader + count * 4 || s[2] <= 0.0 || s[4] <= 0.0) {
        return std::nullopt;
    }
    StabilizerAnalysis a;
    try {
        a.frameRate = Rational(static_cast<std::int64_t>(s[1]), static_cast<std::int64_t>(s[2]));
        a.start = Rational(static_cast<std::int64_t>(s[3]), static_cast<std::int64_t>(s[4]));
    } catch (const std::exception&) {
        return std::nullopt;
    }
    a.width = s[5];
    a.height = s[6];
    a.motions.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto* m = &s[kStabilizerHeader + i * 4];
        a.motions[i] = {m[0], m[1], m[2], m[3]};
    }
    return a;
}

} // namespace loki::render
