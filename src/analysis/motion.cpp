#include "analysis/motion.h"

#include "media/video_decoder.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace loki::analysis {

namespace {

constexpr int kAnalysisWidth = 480; // decode size of the analyses (enough for global motion)
constexpr int kBlock = 16;
constexpr int kGridStep = 20;

struct Vector {
    double x = 0.0; // block center (previous image)
    double y = 0.0;
    double dx = 0.0;
    double dy = 0.0;
};

std::uint64_t sad(const GrayImage& a, int ax, int ay, const GrayImage& b, int bx, int by, int size) {
    std::uint64_t sum = 0;
    for (int y = 0; y < size; ++y) {
        const auto* pa = &a.pixels[static_cast<std::size_t>(ay + y) * static_cast<std::size_t>(a.width) + static_cast<std::size_t>(ax)];
        const auto* pb = &b.pixels[static_cast<std::size_t>(by + y) * static_cast<std::size_t>(b.width) + static_cast<std::size_t>(bx)];
        for (int x = 0; x < size; ++x) {
            sum += static_cast<std::uint64_t>(std::abs(static_cast<int>(pa[x]) - static_cast<int>(pb[x])));
        }
    }
    return sum;
}

// Sub-pixel offset of a minimum from its two neighbours' costs (parabola through three points).
double parabola(double left, double mid, double right) {
    const double denom = left - 2.0 * mid + right;
    return denom > 1e-9 ? std::clamp(0.5 * (left - right) / denom, -0.5, 0.5) : 0.0;
}

double variance(const GrayImage& img, int x0, int y0, int size) {
    double sum = 0.0;
    double sq = 0.0;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const double v = img.at(x0 + x, y0 + y);
            sum += v;
            sq += v * v;
        }
    }
    const double n = static_cast<double>(size) * size;
    return sq / n - (sum / n) * (sum / n);
}

struct Similarity {
    double a = 1.0; // scale * cos
    double b = 0.0; // scale * sin
    double tx = 0.0;
    double ty = 0.0;
};

Similarity fit(const std::vector<Vector>& v) {
    Similarity s;
    if (v.empty()) {
        return s;
    }
    double cx = 0.0;
    double cy = 0.0;
    double qx = 0.0;
    double qy = 0.0;
    for (const auto& p : v) {
        cx += p.x;
        cy += p.y;
        qx += p.x + p.dx;
        qy += p.y + p.dy;
    }
    const double n = static_cast<double>(v.size());
    cx /= n;
    cy /= n;
    qx /= n;
    qy /= n;
    double num1 = 0.0;
    double num2 = 0.0;
    double den = 0.0;
    for (const auto& p : v) {
        const double px = p.x - cx;
        const double py = p.y - cy;
        const double tx = p.x + p.dx - qx;
        const double ty = p.y + p.dy - qy;
        num1 += px * tx + py * ty;
        num2 += px * ty - py * tx;
        den += px * px + py * py;
    }
    if (den > 1e-9 && v.size() >= 3) {
        s.a = num1 / den;
        s.b = num2 / den;
    }
    s.tx = qx - (s.a * cx - s.b * cy);
    s.ty = qy - (s.b * cx + s.a * cy);
    return s;
}

double residual(const Similarity& s, const Vector& p) {
    const double ex = s.a * p.x - s.b * p.y + s.tx - (p.x + p.dx);
    const double ey = s.b * p.x + s.a * p.y + s.ty - (p.y + p.dy);
    return std::sqrt(ex * ex + ey * ey);
}

double sampleBilinear(const GrayImage& img, double x, double y) {
    x = std::clamp(x, 0.0, static_cast<double>(img.width - 1));
    y = std::clamp(y, 0.0, static_cast<double>(img.height - 1));
    const int x0 = static_cast<int>(x);
    const int y0 = static_cast<int>(y);
    const int x1 = std::min(x0 + 1, img.width - 1);
    const int y1 = std::min(y0 + 1, img.height - 1);
    const double fx = x - x0;
    const double fy = y - y0;
    const double top = img.at(x0, y0) + (img.at(x1, y0) - img.at(x0, y0)) * fx;
    const double bottom = img.at(x0, y1) + (img.at(x1, y1) - img.at(x0, y1)) * fx;
    return top + (bottom - top) * fy;
}

struct DecodedRange {
    std::vector<GrayImage> images;
    std::vector<Rational> times;
    double toMedia = 1.0; // analysis pixel -> media pixel
    double mediaWidth = 0.0;
    double mediaHeight = 0.0;
    Rational frameRate;
};

std::optional<DecodedRange> decodeRange(const std::filesystem::path& path, const Rational& from, const Rational& to,
                                        const Progress& progress, const std::atomic<bool>& cancel, double progressShare) {
    media::VideoDecoderOptions options;
    options.allowHardware = false;
    options.maxWidth = kAnalysisWidth;
    options.maxHeight = kAnalysisWidth;
    media::VideoDecoder decoder(path, options);
    DecodedRange out;
    out.mediaWidth = decoder.info().width;
    out.mediaHeight = decoder.info().height;
    out.frameRate = decoder.info().frameRate.isPositive() ? decoder.info().frameRate : Rational(30);
    decoder.seek(from);
    const double span = std::max(1e-6, (to - from).toDouble());
    while (auto frame = decoder.nextFrame()) {
        if (cancel.load()) {
            return std::nullopt;
        }
        if (frame->pts + frame->duration <= from) {
            continue;
        }
        if (frame->pts >= to) {
            break;
        }
        out.images.push_back(lumaOf(*frame, kAnalysisWidth));
        out.times.push_back(frame->pts);
        if (progress) {
            progress(std::clamp((frame->pts - from).toDouble() / span, 0.0, 1.0) * progressShare);
        }
    }
    if (!out.images.empty() && out.images.front().width > 0) {
        out.toMedia = out.mediaWidth / out.images.front().width;
    }
    return out;
}

} // namespace

GrayImage lumaOf(const media::VideoFrame& frame, int maxWidth) {
    GrayImage out;
    if (frame.width <= 0 || frame.height <= 0 || frame.planeCount == 0) {
        return out;
    }
    const int k = std::max(1, (frame.width + maxWidth - 1) / std::max(1, maxWidth));
    out.width = frame.width / k;
    out.height = frame.height / k;
    out.pixels.resize(static_cast<std::size_t>(out.width) * static_cast<std::size_t>(out.height));
    const bool rgba = frame.format == media::PixelFormat::Rgba;
    const auto& plane = frame.planes[0];
    const auto stride = static_cast<std::size_t>(frame.strides[0]);
    for (int y = 0; y < out.height; ++y) {
        for (int x = 0; x < out.width; ++x) {
            int sum = 0;
            for (int yy = 0; yy < k; ++yy) {
                const auto row = static_cast<std::size_t>(y * k + yy) * stride;
                for (int xx = 0; xx < k; ++xx) {
                    const auto px = static_cast<std::size_t>(x * k + xx);
                    if (rgba) {
                        const auto i = row + px * 4;
                        sum += (54 * plane[i] + 183 * plane[i + 1] + 19 * plane[i + 2]) >> 8;
                    } else {
                        sum += plane[row + px];
                    }
                }
            }
            out.pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(out.width) + static_cast<std::size_t>(x)] =
                static_cast<std::uint8_t>(sum / (k * k));
        }
    }
    return out;
}

render::FrameMotion estimateMotion(const GrayImage& prev, const GrayImage& cur, int R) {
    render::FrameMotion motion;
    if (prev.width != cur.width || prev.height != cur.height || prev.width < kBlock + 2 * R + 2 ||
        prev.height < kBlock + 2 * R + 2) {
        return motion;
    }
    std::vector<Vector> vectors;
    for (int y0 = R + 1; y0 + kBlock + R + 1 <= prev.height; y0 += kGridStep) {
        for (int x0 = R + 1; x0 + kBlock + R + 1 <= prev.width; x0 += kGridStep) {
            if (variance(prev, x0, y0, kBlock) < 25.0) {
                continue; // flat: no reliable match
            }
            std::uint64_t best = std::numeric_limits<std::uint64_t>::max();
            int bx = 0;
            int by = 0;
            std::vector<std::uint64_t> costs(static_cast<std::size_t>((2 * R + 1) * (2 * R + 1)));
            for (int dy = -R; dy <= R; ++dy) {
                for (int dx = -R; dx <= R; ++dx) {
                    const auto c = sad(prev, x0, y0, cur, x0 + dx, y0 + dy, kBlock);
                    costs[static_cast<std::size_t>((dy + R) * (2 * R + 1) + dx + R)] = c;
                    if (c < best) {
                        best = c;
                        bx = dx;
                        by = dy;
                    }
                }
            }
            const auto cost = [&](int dx, int dy) {
                return static_cast<double>(costs[static_cast<std::size_t>((dy + R) * (2 * R + 1) + dx + R)]);
            };
            double sx = bx;
            double sy = by;
            if (bx > -R && bx < R) {
                sx += parabola(cost(bx - 1, by), cost(bx, by), cost(bx + 1, by));
            }
            if (by > -R && by < R) {
                sy += parabola(cost(bx, by - 1), cost(bx, by), cost(bx, by + 1));
            }
            vectors.push_back({x0 + kBlock / 2.0, y0 + kBlock / 2.0, sx, sy});
        }
    }
    if (vectors.empty()) {
        return motion;
    }
    // Fit, drop outliers (moving subjects), refit.
    auto s = fit(vectors);
    std::vector<double> residuals;
    for (const auto& v : vectors) {
        residuals.push_back(residual(s, v));
    }
    auto sorted = residuals;
    std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(sorted.size() / 2), sorted.end());
    const double limit = std::max(0.75, 2.0 * sorted[sorted.size() / 2]);
    std::vector<Vector> inliers;
    for (std::size_t i = 0; i < vectors.size(); ++i) {
        if (residuals[i] <= limit) {
            inliers.push_back(vectors[i]);
        }
    }
    if (inliers.size() >= 3) {
        s = fit(inliers);
    }
    // Express as rotation/scale about the picture center followed by a translation.
    const double cx = prev.width / 2.0;
    const double cy = prev.height / 2.0;
    motion.scale = std::sqrt(s.a * s.a + s.b * s.b);
    motion.rotation = std::atan2(s.b, s.a);
    motion.dx = s.a * cx - s.b * cy + s.tx - cx;
    motion.dy = s.b * cx + s.a * cy + s.ty - cy;
    return motion;
}

RegionMotion trackRegion(const GrayImage& prev, const GrayImage& cur, double x, double y, double w, double h, int R,
                         bool withScale) {
    RegionMotion best;
    if (prev.width <= 0 || w < 2 || h < 2) {
        return best;
    }
    const double cx = x + w / 2.0;
    const double cy = y + h / 2.0;
    const int step = std::max(1, static_cast<int>(std::max(w, h) / 48.0)); // bound the samples
    double bestCost = std::numeric_limits<double>::max();
    const double scales[3] = {1.0, 0.96, 1.04};
    for (const double scale : withScale ? std::vector<double>{scales[0], scales[1], scales[2]} : std::vector<double>{1.0}) {
        for (int dy = -R; dy <= R; ++dy) {
            for (int dx = -R; dx <= R; ++dx) {
                double cost = 0.0;
                int count = 0;
                for (double py = y; py < y + h; py += step) {
                    for (double px = x; px < x + w; px += step) {
                        const double qx = cx + (px - cx) * scale + dx;
                        const double qy = cy + (py - cy) * scale + dy;
                        cost += std::abs(sampleBilinear(prev, px, py) - sampleBilinear(cur, qx, qy));
                        ++count;
                    }
                }
                cost /= std::max(1, count);
                // Prefer no motion and no scale on ties (flat regions).
                cost += 1e-3 * (std::abs(dx) + std::abs(dy)) + (scale != 1.0 ? 0.05 : 0.0);
                if (cost < bestCost) {
                    bestCost = cost;
                    best = {static_cast<double>(dx), static_cast<double>(dy), scale};
                }
            }
        }
    }
    return best;
}

std::optional<render::StabilizerAnalysis> analyzeStabilizer(const std::filesystem::path& path, const Rational& from,
                                                            const Rational& to, const Progress& progress,
                                                            const std::atomic<bool>& cancel) {
    auto range = decodeRange(path, from, to, progress, cancel, 0.5);
    if (!range || range->images.empty()) {
        return std::nullopt;
    }
    render::StabilizerAnalysis analysis;
    analysis.frameRate = range->frameRate;
    analysis.start = range->times.front();
    analysis.width = range->mediaWidth;
    analysis.height = range->mediaHeight;
    analysis.motions.resize(range->images.size());
    for (std::size_t i = 1; i < range->images.size(); ++i) {
        if (cancel.load()) {
            return std::nullopt;
        }
        auto m = estimateMotion(range->images[i - 1], range->images[i]);
        m.dx *= range->toMedia;
        m.dy *= range->toMedia;
        analysis.motions[i] = m;
        if (progress) {
            progress(0.5 + 0.5 * static_cast<double>(i) / static_cast<double>(range->images.size()));
        }
    }
    return analysis;
}

std::optional<std::vector<model::MaskPathKey>> trackMask(const std::filesystem::path& path,
                                                         const std::vector<model::MaskVertex>& start, const Rational& from,
                                                         const Rational& to, const Progress& progress,
                                                         const std::atomic<bool>& cancel) {
    const bool forward = from <= to;
    const Rational lo = forward ? from : to;
    const Rational hi = forward ? to : from;
    auto range = decodeRange(path, lo, hi, progress, cancel, 0.5);
    if (!range || range->images.empty() || start.size() < 3) {
        return std::nullopt;
    }
    const std::size_t n = range->images.size();
    // Start at the frame nearest `from` and walk in the tracking direction.
    std::size_t index = forward ? 0 : n - 1;
    std::vector<model::MaskPathKey> keys;
    auto current = start;
    keys.push_back({range->times[index], current});
    const double k = range->toMedia;
    for (std::size_t step = 1; step < n; ++step) {
        if (cancel.load()) {
            return std::nullopt;
        }
        const std::size_t next = forward ? index + 1 : index - 1;
        double minX = current.front().x;
        double maxX = minX;
        double minY = current.front().y;
        double maxY = minY;
        for (const auto& v : current) {
            minX = std::min(minX, v.x);
            maxX = std::max(maxX, v.x);
            minY = std::min(minY, v.y);
            maxY = std::max(maxY, v.y);
        }
        const auto m = trackRegion(range->images[index], range->images[next], minX / k, minY / k, (maxX - minX) / k,
                                   (maxY - minY) / k, 6, true);
        current = model::transformedPath(current, m.dx * k, m.dy * k, m.scale, (minX + maxX) / 2.0, (minY + maxY) / 2.0);
        index = next;
        keys.push_back({range->times[index], current});
        if (progress) {
            progress(0.5 + 0.5 * static_cast<double>(step) / static_cast<double>(n));
        }
    }
    std::sort(keys.begin(), keys.end(), [](const auto& a, const auto& b) { return a.time < b.time; });
    return keys;
}

} // namespace loki::analysis
