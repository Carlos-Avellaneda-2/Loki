// Phase 13 render core: straight-alpha compositing for nested sequences (D30), adjustment layers
// (D31) and video scopes (D32).
#include "render/cpu_compositor.h"
#include "render/scopes.h"

#include <gtest/gtest.h>

#include <memory>

namespace {

using namespace loki;
using loki::model::BlendMode;

media::VideoFramePtr solid(int w, int h, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    auto f = std::make_shared<media::VideoFrame>();
    f->format = media::PixelFormat::Rgba;
    f->width = w;
    f->height = h;
    f->planeCount = 1;
    f->strides = {w * 4, 0, 0};
    f->planeHeights = {h, 0, 0};
    f->planes[0].resize(static_cast<std::size_t>(w * h * 4));
    for (std::size_t i = 0; i < f->planes[0].size(); i += 4) {
        f->planes[0][i] = r;
        f->planes[0][i + 1] = g;
        f->planes[0][i + 2] = b;
        f->planes[0][i + 3] = 255;
    }
    return f;
}

render::Layer layerOf(media::VideoFramePtr frame, double x, double y, double opacity = 100) {
    render::Layer l;
    l.sourceWidth = frame->width;
    l.sourceHeight = frame->height;
    l.frame = std::move(frame);
    l.transform.positionX = x;
    l.transform.positionY = y;
    l.transform.anchorX = l.sourceWidth / 2;
    l.transform.anchorY = l.sourceHeight / 2;
    l.transform.opacity = opacity;
    return l;
}

TEST(CompositeOverTest, OpaqueDestinationEqualsTheClassicStep) {
    const render::Rgb src{0.8f, 0.2f, 0.5f};
    const render::Rgb dst{0.1f, 0.6f, 0.3f};
    for (int mode = 0; mode < model::kBlendModeCount; ++mode) {
        const auto m = static_cast<BlendMode>(mode);
        const auto classic = render::compositePixel(m, src, dst, 0.4f);
        const auto over = render::compositeOver(m, src, 0.4f, {dst, 1.0f});
        EXPECT_NEAR(over.alpha, 1.0f, 1e-6f);
        for (std::size_t c = 0; c < 3; ++c) {
            EXPECT_NEAR(over.rgb[c], classic[c], 1e-5f) << "mode " << mode;
        }
    }
}

TEST(CompositeOverTest, TransparentDestinationKeepsTheSourceColor) {
    const auto over = render::compositeOver(BlendMode::Multiply, {0.8f, 0.2f, 0.5f}, 0.5f, {});
    EXPECT_NEAR(over.alpha, 0.5f, 1e-6f);
    EXPECT_NEAR(over.rgb[0], 0.8f, 1e-6f); // no multiply against nothing
}

TEST(CpuCompositorTest, NestedCompositeShowsTheBackgroundWhereEmpty) {
    auto inner = std::make_shared<render::CompositeFrame>();
    inner->width = 20;
    inner->height = 10;
    inner->transparent = true;
    inner->layers.push_back(layerOf(solid(10, 10, 255, 0, 0), 5, 5)); // left half red
    render::Layer nest;
    nest.nested = inner;
    nest.sourceWidth = 20;
    nest.sourceHeight = 10;
    nest.transform.positionX = 10;
    nest.transform.positionY = 5;
    nest.transform.anchorX = 10;
    nest.transform.anchorY = 5;
    nest.transform.opacity = 100;
    render::CompositeFrame frame{20, 10, {layerOf(solid(20, 10, 0, 0, 255), 10, 5), nest}};
    const auto img = render::compositeCpu(frame, 20, 10);
    EXPECT_EQ(img.pixel(2, 5), (std::array<int, 3>{255, 0, 0}));
    EXPECT_EQ(img.pixel(15, 5), (std::array<int, 3>{0, 0, 255}));
}

TEST(CpuCompositorTest, AdjustmentLayerGradesEverythingBelow) {
    render::Layer adjustment;
    adjustment.adjustment = true;
    render::coverCanvas(adjustment, 20, 10);
    adjustment.transform.opacity = 100;
    adjustment.grade.color.saturation = 0.0f; // black & white
    render::CompositeFrame frame{20, 10,
                                 {layerOf(solid(10, 10, 255, 0, 0), 5, 5), layerOf(solid(10, 10, 0, 255, 0), 15, 5),
                                  adjustment}};
    const auto img = render::compositeCpu(frame, 20, 10);
    const auto left = img.pixel(2, 5);
    const auto right = img.pixel(17, 5);
    EXPECT_EQ(left[0], left[1]);
    EXPECT_EQ(left[1], left[2]);
    EXPECT_EQ(right[0], right[2]);
    EXPECT_GT(right[0], left[0]); // green is brighter than red in Rec.709 luma
}

TEST(ScopesTest, LumaAndChromaOfPrimaries) {
    const auto white = render::toYCbCr709(255, 255, 255);
    EXPECT_EQ(white.y, 255);
    EXPECT_EQ(white.cb, 128);
    EXPECT_EQ(white.cr, 128);
    const auto red = render::toYCbCr709(255, 0, 0);
    EXPECT_EQ(red.y, 54);
    EXPECT_GT(red.cr, 200);
    EXPECT_LT(red.cb, 128);
}

TEST(ScopesTest, WaveformParadeVectorscopeAndHistogramCountEveryPixel) {
    // 4x2 image: left half black, right half pure red.
    const int w = 4;
    const int h = 2;
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(w * h * 4), 0);
    for (int y = 0; y < h; ++y) {
        for (int x = 2; x < 4; ++x) {
            rgba[static_cast<std::size_t>((y * w + x) * 4)] = 255;
        }
        for (int x = 0; x < w; ++x) {
            rgba[static_cast<std::size_t>((y * w + x) * 4 + 3)] = 255;
        }
    }
    const auto d = render::analyzeScopes(rgba.data(), w, h, w * 4, 2);
    ASSERT_EQ(d.columns, 2);
    EXPECT_EQ(d.samples, 8u);
    // Column 0 is black (level 0), column 1 is red (luma 54, R 255).
    EXPECT_EQ(d.luma[0 * 2 + 0], 4u);
    EXPECT_EQ(d.luma[54 * 2 + 1], 4u);
    EXPECT_EQ(d.parade[0][255 * 2 + 1], 4u);
    EXPECT_EQ(d.parade[1][0 * 2 + 1], 4u);
    EXPECT_EQ(d.histogram[0][255], 4u);
    EXPECT_EQ(d.histogram[0][0], 4u);
    EXPECT_EQ(d.histogram[3][54], 4u);
    const auto red = render::toYCbCr709(255, 0, 0);
    EXPECT_EQ(d.vectorscope[static_cast<std::size_t>(red.cr) * 256 + static_cast<std::size_t>(red.cb)], 4u);
    EXPECT_EQ(d.vectorscope[128 * 256 + 128], 4u);
}

} // namespace
