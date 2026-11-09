#include "render/cpu_compositor.h"
#include "render/grading.h"

#include <gtest/gtest.h>

#include <sstream>

namespace {

using namespace loki;
using namespace loki::render;

media::VideoFramePtr solid(int w, int h, std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a = 255) {
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
        f->planes[0][i + 3] = a;
    }
    return f;
}

Layer full(media::VideoFramePtr frame) {
    Layer l;
    l.sourceWidth = frame->width;
    l.sourceHeight = frame->height;
    l.frame = std::move(frame);
    l.transform.positionX = l.sourceWidth / 2;
    l.transform.positionY = l.sourceHeight / 2;
    l.transform.anchorX = l.sourceWidth / 2;
    l.transform.anchorY = l.sourceHeight / 2;
    return l;
}

// Identity 2x2x2 cube with a red channel inverted.
std::string invertRedCube() {
    std::ostringstream s;
    s << "TITLE \"test\"\n# comment\nLUT_3D_SIZE 2\n";
    for (int b = 0; b < 2; ++b) {
        for (int g = 0; g < 2; ++g) {
            for (int r = 0; r < 2; ++r) {
                s << (1 - r) << ' ' << g << ' ' << b << '\n';
            }
        }
    }
    return s.str();
}

TEST(GradingTest, ColorAdjustments) {
    const Rgb gray{0.25f, 0.25f, 0.25f};
    ColorAdjust a;
    EXPECT_TRUE(a.isIdentity());
    a.exposure = 1.0f; // one stop = x2
    EXPECT_NEAR(adjustColor(a, gray)[0], 0.5f, 1e-6f);
    a = {};
    a.contrast = -100.0f; // flat mid gray
    EXPECT_NEAR(adjustColor(a, {0.1f, 0.9f, 0.4f})[1], 0.5f, 1e-6f);
    a = {};
    a.saturation = 0.0f; // luma only
    const auto bw = adjustColor(a, {1.0f, 0.0f, 0.0f});
    EXPECT_NEAR(bw[0], 0.2126f, 1e-5f);
    EXPECT_NEAR(bw[1], 0.2126f, 1e-5f);
    a = {};
    a.temperature = 100.0f; // warmer
    const auto warm = adjustColor(a, gray);
    EXPECT_GT(warm[0], gray[0]);
    EXPECT_LT(warm[2], gray[2]);
}

TEST(GradingTest, CubeLutParsingAndSampling) {
    const auto lut = parseCubeLut(invertRedCube());
    ASSERT_EQ(lut->size, 2);
    const auto c = lut->sample({0.25f, 0.5f, 0.75f});
    EXPECT_NEAR(c[0], 0.75f, 1e-5f); // trilinear: 1 - r
    EXPECT_NEAR(c[1], 0.5f, 1e-5f);
    EXPECT_NEAR(c[2], 0.75f, 1e-5f);
    EXPECT_THROW((void)parseCubeLut("LUT_3D_SIZE 2\n0 0 0\n"), std::runtime_error);   // too few rows
    EXPECT_THROW((void)parseCubeLut("0 0 0\n"), std::runtime_error);                   // no size
    EXPECT_THROW((void)parseCubeLut("LUT_1D_SIZE 4\n"), std::runtime_error);           // 1D
    EXPECT_THROW((void)parseCubeLut("LUT_3D_SIZE 2\nfoo bar\n"), std::runtime_error);  // junk
    LayerGrade g;
    g.lut = lut;
    g.lutMix = 0.5f;
    EXPECT_NEAR(gradePixel(g, {0.0f, 0.0f, 0.0f})[0], 0.5f, 1e-5f);
    g.dim = 1.0f;
    EXPECT_EQ(gradePixel(g, {1.0f, 1.0f, 1.0f})[1], 0.0f);
}

TEST(GradingTest, WipeMaskMovesLeftToRight) {
    WipeMask w{true, 0.0f, 0.02f, false};
    EXPECT_EQ(wipeAlpha(w, 0.0f), 0.0f);
    w.progress = 1.0f;
    EXPECT_EQ(wipeAlpha(w, 1.0f), 1.0f);
    w.progress = 0.5f;
    EXPECT_EQ(wipeAlpha(w, 0.2f), 1.0f);
    EXPECT_EQ(wipeAlpha(w, 0.8f), 0.0f);
    w.reverse = true;
    EXPECT_EQ(wipeAlpha(w, 0.8f), 1.0f);
    EXPECT_EQ(wipeAlpha(WipeMask{}, 0.3f), 1.0f);
}

TEST(CpuCompositorTest, LayersGradesWipesAndAlpha) {
    CompositeFrame frame{8, 4, {full(solid(8, 4, 200, 100, 50))}};
    auto img = compositeCpu(frame, 8, 4);
    EXPECT_EQ(img.pixel(1, 1), (std::array<int, 3>{200, 100, 50}));
    // Half-transparent white text pixel (straight alpha) over the base.
    frame.layers.push_back(full(solid(8, 4, 255, 255, 255, 128)));
    img = compositeCpu(frame, 8, 4);
    EXPECT_NEAR(img.pixel(1, 1)[2], 50 + (255 - 50) * 128 / 255.0, 1.5);
    // Wipe on the top layer: left half shows it, right half the base.
    frame.layers[1].frame = solid(8, 4, 0, 0, 255);
    frame.layers[1].wipe = {true, 0.5f, 0.01f, false};
    img = compositeCpu(frame, 8, 4);
    EXPECT_EQ(img.pixel(1, 1), (std::array<int, 3>{0, 0, 255}));
    EXPECT_EQ(img.pixel(6, 1), (std::array<int, 3>{200, 100, 50}));
    // Dip to black on the base only.
    frame.layers.pop_back();
    frame.layers[0].grade.dim = 0.5f;
    img = compositeCpu(frame, 8, 4);
    EXPECT_NEAR(img.pixel(1, 1)[0], 100, 1);
    // Reduced output size samples the whole canvas.
    EXPECT_EQ(compositeCpu(frame, 4, 2).width, 4);
}

} // namespace
