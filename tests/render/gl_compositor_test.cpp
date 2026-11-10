// Runs the real OpenGL compositor and compares its output with the CPU reference. Needs a GPU with
// OpenGL 3.3 (skipped otherwise, e.g. on CI runners without a GPU). This executable runs on the
// native "windows" platform instead of "offscreen" (see tests/CMakeLists.txt).
#include "render/cpu_compositor.h"
#include "render/gl_compositor.h"

#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QSurfaceFormat>

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

render::Layer layerOf(media::VideoFramePtr frame, double x, double y, double opacity = 100,
                      BlendMode blend = BlendMode::Normal) {
    render::Layer l;
    l.sourceWidth = frame->width;
    l.sourceHeight = frame->height;
    l.frame = std::move(frame);
    l.transform.positionX = x;
    l.transform.positionY = y;
    l.transform.anchorX = l.sourceWidth / 2;
    l.transform.anchorY = l.sourceHeight / 2;
    l.transform.opacity = opacity;
    l.transform.blend = blend;
    return l;
}

class GlCompositorTest : public ::testing::Test {
protected:
    QOffscreenSurface surface;
    QOpenGLContext context;
    render::GlCompositor compositor;
    bool ok = false;

    void SetUp() override {
        QSurfaceFormat format;
        format.setVersion(3, 3);
        format.setProfile(QSurfaceFormat::CompatibilityProfile);
        surface.setFormat(format);
        surface.create();
        context.setFormat(format);
        if (!context.create() || !context.makeCurrent(&surface)) {
            GTEST_SKIP() << "No OpenGL context available";
        }
        const auto f = context.format();
        if (f.majorVersion() * 10 + f.minorVersion() < 33 || !compositor.initialize()) {
            GTEST_SKIP() << "OpenGL 3.3 not available";
        }
        ok = true;
    }

    void TearDown() override {
        if (ok) {
            compositor.release();
            context.doneCurrent();
        }
    }

    static QColor at(const QImage& img, int x, int y) { return img.pixelColor(x, y); }
};

TEST_F(GlCompositorTest, EmptyCompositeIsBlack) {
    render::CompositeFrame frame{64, 36, {}};
    compositor.render(frame, 64, 36);
    const auto img = compositor.readback();
    ASSERT_EQ(img.width(), 64);
    EXPECT_EQ(at(img, 10, 10), QColor(0, 0, 0));
}

TEST_F(GlCompositorTest, PlacesLayerWithTransform) {
    // 20x20 red square centered at (16, 10) in a 64x36 canvas: covers x 6..25, y 0..19.
    render::CompositeFrame frame{64, 36, {layerOf(solid(20, 20, 255, 0, 0), 16, 10)}};
    compositor.render(frame, 64, 36);
    const auto img = compositor.readback();
    EXPECT_EQ(at(img, 16, 10), QColor(255, 0, 0));
    EXPECT_EQ(at(img, 40, 10), QColor(0, 0, 0));
    EXPECT_EQ(at(img, 16, 30), QColor(0, 0, 0));
}

TEST_F(GlCompositorTest, BlendModesAndOpacityMatchCpuReference) {
    const std::uint8_t base[3] = {128, 64, 200};
    const std::uint8_t top[3] = {200, 128, 32};
    const render::Rgb dst{base[0] / 255.0f, base[1] / 255.0f, base[2] / 255.0f};
    const render::Rgb src{top[0] / 255.0f, top[1] / 255.0f, top[2] / 255.0f};
    for (int mode = 0; mode < model::kBlendModeCount; ++mode) {
        for (const double opacity : {100.0, 40.0}) {
            render::CompositeFrame frame{32, 32,
                                         {layerOf(solid(32, 32, base[0], base[1], base[2]), 16, 16),
                                          layerOf(solid(32, 32, top[0], top[1], top[2]), 16, 16, opacity,
                                                  static_cast<BlendMode>(mode))}};
            compositor.render(frame, 32, 32);
            const auto img = compositor.readback();
            const auto expected = render::compositePixel(static_cast<BlendMode>(mode), src, dst,
                                                         static_cast<float>(opacity / 100.0));
            const auto got = at(img, 16, 16);
            EXPECT_NEAR(got.red(), expected[0] * 255.0f, 2.0) << "mode " << mode << " opacity " << opacity;
            EXPECT_NEAR(got.green(), expected[1] * 255.0f, 2.0) << "mode " << mode;
            EXPECT_NEAR(got.blue(), expected[2] * 255.0f, 2.0) << "mode " << mode;
        }
    }
}

TEST_F(GlCompositorTest, RenderAtReducedSizeScalesTheCanvas) {
    render::CompositeFrame frame{64, 36, {layerOf(solid(32, 36, 0, 255, 0), 16, 18)}};
    compositor.render(frame, 32, 18); // half resolution preview
    const auto img = compositor.readback();
    ASSERT_EQ(img.width(), 32);
    EXPECT_EQ(at(img, 4, 9), QColor(0, 255, 0));
    EXPECT_EQ(at(img, 28, 9), QColor(0, 0, 0));
}

TEST_F(GlCompositorTest, GradingWipeLutAndAlphaMatchCpuCompositor) {
    std::string cube = "LUT_3D_SIZE 2\n";
    for (int b = 0; b < 2; ++b) {
        for (int g = 0; g < 2; ++g) {
            for (int r = 0; r < 2; ++r) {
                cube += std::to_string(1 - r) + " " + std::to_string(g) + " " + std::to_string(b) + "\n";
            }
        }
    }
    auto base = layerOf(solid(64, 32, 180, 90, 40), 32, 16);
    base.grade.color = {0.5f, 20.0f, 60.0f, 30.0f};
    base.grade.lut = render::parseCubeLut(cube);
    base.grade.lutMix = 0.7f;
    auto top = layerOf(solid(64, 32, 20, 200, 120), 32, 16, 80.0);
    top.wipe = {true, 0.5f, 0.05f, false};
    top.grade.dim = 0.3f;
    render::CompositeFrame frame{64, 32, {base, top}};
    compositor.render(frame, 64, 32);
    const auto img = compositor.readback();
    const auto cpu = render::compositeCpu(frame, 64, 32);
    for (const int x : {4, 20, 44, 60}) {
        const auto expected = cpu.pixel(x, 16);
        const auto got = at(img, x, 16);
        EXPECT_NEAR(got.red(), expected[0], 3) << "x " << x;
        EXPECT_NEAR(got.green(), expected[1], 3) << "x " << x;
        EXPECT_NEAR(got.blue(), expected[2], 3) << "x " << x;
    }
}

// Nested sequences (D30): a transparent sub-composite whose empty area shows the layers below,
// placed with its own transform and opacity; blend modes apply only over opaque pixels.
TEST_F(GlCompositorTest, NestedTransparentCompositeMatchesCpuCompositor) {
    auto inner = std::make_shared<render::CompositeFrame>();
    inner->width = 32;
    inner->height = 32;
    inner->transparent = true;
    inner->layers.push_back(layerOf(solid(16, 32, 30, 220, 60), 8, 16)); // left half only
    render::Layer nested;
    nested.nested = inner;
    nested.sourceWidth = 32;
    nested.sourceHeight = 32;
    nested.transform.positionX = 32;
    nested.transform.positionY = 16;
    nested.transform.anchorX = 16;
    nested.transform.anchorY = 16;
    nested.transform.opacity = 70;
    nested.transform.blend = BlendMode::Screen;
    render::CompositeFrame frame{64, 32, {layerOf(solid(64, 32, 200, 40, 90), 32, 16), nested}};
    compositor.render(frame, 64, 32);
    const auto img = compositor.readback();
    const auto cpu = render::compositeCpu(frame, 64, 32);
    for (const int x : {4, 20, 28, 40, 60}) {
        const auto expected = cpu.pixel(x, 16);
        const auto got = at(img, x, 16);
        EXPECT_NEAR(got.red(), expected[0], 3) << "x " << x;
        EXPECT_NEAR(got.green(), expected[1], 3) << "x " << x;
        EXPECT_NEAR(got.blue(), expected[2], 3) << "x " << x;
    }
    // Inside the nest's transparent half the background is untouched.
    EXPECT_EQ(cpu.pixel(40, 16), (std::array<int, 3>{200, 40, 90}));
}

// Adjustment layers (D31): the grade applies to everything below, mixed by opacity.
TEST_F(GlCompositorTest, AdjustmentLayerMatchesCpuCompositor) {
    render::Layer adjustment;
    adjustment.adjustment = true;
    render::coverCanvas(adjustment, 64, 32);
    adjustment.transform.opacity = 60;
    adjustment.grade.color = {0.3f, 15.0f, 20.0f, -40.0f};
    render::CompositeFrame frame{64, 32,
                                 {layerOf(solid(32, 32, 180, 90, 40), 16, 16), layerOf(solid(32, 32, 20, 160, 220), 48, 16),
                                  adjustment}};
    compositor.render(frame, 64, 32);
    const auto img = compositor.readback();
    const auto cpu = render::compositeCpu(frame, 64, 32);
    for (const int x : {8, 30, 40, 60}) {
        const auto expected = cpu.pixel(x, 16);
        const auto got = at(img, x, 16);
        EXPECT_NEAR(got.red(), expected[0], 3) << "x " << x;
        EXPECT_NEAR(got.green(), expected[1], 3) << "x " << x;
        EXPECT_NEAR(got.blue(), expected[2], 3) << "x " << x;
    }
}

} // namespace

int main(int argc, char* argv[]) {
    QGuiApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
