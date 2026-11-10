// Phase 14 analyses: global motion (stabilizer, D36) and region tracking (masks, D34).
#include "analysis/motion.h"

#include "media_assets.h"

#include <gtest/gtest.h>

#include <cmath>
#include <random>

namespace {

using namespace loki;
using namespace loki::analysis;

// Smooth random texture (blurred noise) so block matching has something to lock on.
GrayImage texture(int w, int h, unsigned seed) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> dist(0, 255);
    std::vector<double> noise(static_cast<std::size_t>(w * h));
    for (auto& v : noise) {
        v = dist(rng);
    }
    GrayImage img{w, h, std::vector<std::uint8_t>(static_cast<std::size_t>(w * h))};
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            double sum = 0;
            int n = 0;
            for (int dy = -2; dy <= 2; ++dy) {
                for (int dx = -2; dx <= 2; ++dx) {
                    const int sx = std::clamp(x + dx, 0, w - 1);
                    const int sy = std::clamp(y + dy, 0, h - 1);
                    sum += noise[static_cast<std::size_t>(sy * w + sx)];
                    ++n;
                }
            }
            img.pixels[static_cast<std::size_t>(y * w + x)] = static_cast<std::uint8_t>(sum / n);
        }
    }
    return img;
}

// `src` moved by (dx, dy) and rotated by `angle` around the center (content motion).
GrayImage moved(const GrayImage& src, double dx, double dy, double angle = 0.0) {
    GrayImage out{src.width, src.height, std::vector<std::uint8_t>(src.pixels.size())};
    const double cx = src.width / 2.0;
    const double cy = src.height / 2.0;
    const double c = std::cos(angle);
    const double s = std::sin(angle);
    for (int y = 0; y < src.height; ++y) {
        for (int x = 0; x < src.width; ++x) {
            // Inverse: where did this pixel come from?
            const double px = x - dx - cx;
            const double py = y - dy - cy;
            const double sx = c * px + s * py + cx;
            const double sy = -s * px + c * py + cy;
            const int ix = std::clamp(static_cast<int>(std::lround(sx)), 0, src.width - 1);
            const int iy = std::clamp(static_cast<int>(std::lround(sy)), 0, src.height - 1);
            out.pixels[static_cast<std::size_t>(y * src.width + x)] = src.at(ix, iy);
        }
    }
    return out;
}

TEST(MotionTest, RecoversTranslation) {
    const auto a = texture(240, 136, 7);
    const auto m = estimateMotion(a, moved(a, 3.0, -2.0));
    EXPECT_NEAR(m.dx, 3.0, 0.35);
    EXPECT_NEAR(m.dy, -2.0, 0.35);
    EXPECT_NEAR(m.rotation, 0.0, 0.01);
    EXPECT_NEAR(m.scale, 1.0, 0.01);
}

TEST(MotionTest, RecoversSmallRotation) {
    const auto a = texture(240, 136, 11);
    const auto m = estimateMotion(a, moved(a, 0.0, 0.0, 0.03));
    EXPECT_NEAR(m.rotation, 0.03, 0.01);
    EXPECT_NEAR(m.dx, 0.0, 0.6);
}

TEST(MotionTest, FlatPicturesGiveNoMotion) {
    GrayImage flat{120, 80, std::vector<std::uint8_t>(120 * 80, 128)};
    const auto m = estimateMotion(flat, flat);
    EXPECT_EQ(m.dx, 0.0);
    EXPECT_EQ(m.scale, 1.0);
}

TEST(TrackTest, FollowsAMovingRegion) {
    const auto a = texture(160, 120, 3);
    const auto r = trackRegion(a, moved(a, -4.0, 2.0), 50, 40, 40, 30, 6, true);
    EXPECT_EQ(r.dx, -4.0);
    EXPECT_EQ(r.dy, 2.0);
    EXPECT_EQ(r.scale, 1.0);
}

TEST(AnalysisJobTest, StabilizerAnalysesEveryFrameAndCanBeCancelled) {
    const auto& assets = test::MediaAssets::get();
    std::atomic<bool> cancel{false};
    double last = 0.0;
    const auto result = analyzeStabilizer(assets.mpeg4WithAudio, Rational(0), Rational(1), [&](double p) { last = p; }, cancel);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->motions.size(), 25u); // 25 fps, 1 s
    EXPECT_EQ(result->frameRate, Rational(25));
    EXPECT_GT(last, 0.9);
    EXPECT_GT(result->width, 0.0);
    cancel = true;
    EXPECT_FALSE(analyzeStabilizer(assets.mpeg4WithAudio, Rational(0), Rational(1), {}, cancel).has_value());
}

TEST(AnalysisJobTest, MaskTrackingMakesAKeyPerFrameInBothDirections) {
    const auto& assets = test::MediaAssets::get();
    std::atomic<bool> cancel{false};
    const auto path = model::rectanglePath(40, 30, 50, 40);
    const auto forward = trackMask(assets.mpeg4WithAudio, path, Rational(0), Rational(1, 2), {}, cancel);
    ASSERT_TRUE(forward.has_value());
    EXPECT_GE(forward->size(), 12u);
    EXPECT_TRUE(std::is_sorted(forward->begin(), forward->end(), [](const auto& a, const auto& b) { return a.time < b.time; }));
    const auto backward = trackMask(assets.mpeg4WithAudio, path, Rational(1), Rational(1, 2), {}, cancel);
    ASSERT_TRUE(backward.has_value());
    EXPECT_EQ(backward->back().path, path); // the start key is at `from` (the latest time)
}

} // namespace
