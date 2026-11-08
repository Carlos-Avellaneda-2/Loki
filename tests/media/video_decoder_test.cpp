#include "media/video_decoder.h"

#include "media_assets.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <random>

namespace {

using namespace loki;
using namespace loki::media;
using loki::test::expectedLuma;
using loki::test::MediaAssets;

// Mean luma of the frame's Y plane (or green channel for RGBA).
int meanLuma(const VideoFrame& f) {
    const auto& y = f.planes[0];
    long long sum = 0;
    std::size_t n = 0;
    const std::size_t step = f.format == PixelFormat::Rgba ? 4 : 1;
    for (std::size_t i = f.format == PixelFormat::Rgba ? 1 : 0; i < y.size(); i += step * 7) {
        sum += y[i];
        ++n;
    }
    return static_cast<int>(sum / static_cast<long long>(std::max<std::size_t>(1, n)));
}

std::int64_t frameIndex(const VideoFrame& f, const Rational& rate) {
    return timeToFrames(f.pts, rate, Rounding::Nearest);
}

void expectFrame(const VideoFramePtr& f, std::int64_t index, const Rational& rate) {
    ASSERT_NE(f, nullptr) << "frame " << index;
    EXPECT_EQ(frameIndex(*f, rate), index);
    EXPECT_NEAR(meanLuma(*f), expectedLuma(index), 3) << "frame " << index;
}

TEST(VideoDecoderTest, StreamInfo) {
    const auto& a = MediaAssets::get();
    VideoDecoder d(a.mpeg4WithAudio, {.allowHardware = false});
    EXPECT_EQ(d.info().width, 320);
    EXPECT_EQ(d.info().height, 240);
    EXPECT_EQ(d.info().frameRate, Rational(25));
    EXPECT_EQ(d.info().frameDuration, Rational(1, 25));
    EXPECT_EQ(d.info().codec, "mpeg4");
    EXPECT_EQ(d.hwAccel(), HwAccel::None);
}

TEST(VideoDecoderTest, SequentialDecodeReturnsEveryFrameInOrder) {
    const auto& a = MediaAssets::get();
    for (const bool hw : {false, true}) {
        VideoDecoder d(a.mpeg4WithAudio, {.allowHardware = hw});
        for (int i = 0; i < a.mpeg4Spec.frames; ++i) {
            expectFrame(d.nextFrame(), i, a.mpeg4Spec.frameRate);
        }
        EXPECT_EQ(d.nextFrame(), nullptr);
    }
}

TEST(VideoDecoderTest, OutputFormatAndColor) {
    const auto& a = MediaAssets::get();
    VideoDecoder d(a.mpeg4WithAudio, {.allowHardware = false});
    const auto f = d.nextFrame();
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->format, PixelFormat::Yuv420P);
    EXPECT_EQ(f->planeCount, 3);
    EXPECT_EQ(f->strides[0], 320);
    EXPECT_EQ(f->planes[1].size(), 160u * 120u);
    EXPECT_EQ(f->duration, Rational(1, 25));
    EXPECT_EQ(f->range, ColorRange::Limited);
}

TEST(VideoDecoderTest, FrameAtIsExactForRandomAccess) {
    const auto& a = MediaAssets::get();
    for (const auto* spec : {&a.mpeg4Spec, &a.h264Spec}) {
        const auto& file = spec == &a.mpeg4Spec ? a.mpeg4WithAudio : a.h264;
        VideoDecoder d(file, {.allowHardware = false});
        std::mt19937 rng(5);
        for (int i = 0; i < 40; ++i) {
            const auto index = static_cast<std::int64_t>(rng() % static_cast<unsigned>(spec->frames));
            // Ask for a time in the middle of the frame: it must return that frame.
            const Rational t = framesToTime(index, spec->frameRate) + spec->frameRate.reciprocal() / Rational(2);
            expectFrame(d.frameAt(t), index, spec->frameRate);
        }
    }
}

TEST(VideoDecoderTest, FrameAtExactBoundaries) {
    const auto& a = MediaAssets::get();
    VideoDecoder d(a.h264, {.allowHardware = false});
    const auto rate = a.h264Spec.frameRate;
    for (const std::int64_t index : {0LL, 1LL, 14LL, 15LL, 16LL, 30LL, 59LL}) {
        expectFrame(d.frameAt(framesToTime(index, rate)), index, rate);
    }
    // Backwards across GOPs.
    for (std::int64_t index = 59; index >= 0; index -= 7) {
        expectFrame(d.frameAt(framesToTime(index, rate)), index, rate);
    }
}

TEST(VideoDecoderTest, FrameAtPastEndReturnsNull) {
    const auto& a = MediaAssets::get();
    VideoDecoder d(a.mpeg4WithAudio, {.allowHardware = false});
    EXPECT_EQ(d.frameAt(Rational(100)), nullptr);
    // And the decoder still works afterwards.
    expectFrame(d.frameAt(Rational(0)), 0, a.mpeg4Spec.frameRate);
}

TEST(VideoDecoderTest, NegativeTimeClampsToFirstFrame) {
    const auto& a = MediaAssets::get();
    VideoDecoder d(a.mpeg4WithAudio, {.allowHardware = false});
    expectFrame(d.frameAt(Rational(-5)), 0, a.mpeg4Spec.frameRate);
}

TEST(VideoDecoderTest, SeekThenSequential) {
    const auto& a = MediaAssets::get();
    VideoDecoder d(a.mpeg4WithAudio, {.allowHardware = false});
    d.seek(framesToTime(20, a.mpeg4Spec.frameRate));
    for (int i = 20; i < 30; ++i) {
        expectFrame(d.nextFrame(), i, a.mpeg4Spec.frameRate);
    }
}

TEST(VideoDecoderTest, FramesUpToReturnsWholeRunForReversePlayback) {
    const auto& a = MediaAssets::get();
    VideoDecoder d(a.h264, {.allowHardware = false});
    const auto rate = a.h264Spec.frameRate;
    const auto frames = d.framesUpTo(framesToTime(20, rate));
    ASSERT_FALSE(frames.empty());
    EXPECT_EQ(frameIndex(*frames.back(), rate), 20);
    EXPECT_LE(frameIndex(*frames.front(), rate), 15); // starts at a keyframe (GOP 15)
    for (std::size_t i = 1; i < frames.size(); ++i) {
        EXPECT_EQ(frameIndex(*frames[i], rate), frameIndex(*frames[i - 1], rate) + 1);
    }
}

TEST(VideoDecoderTest, DownscalesKeepingAspect) {
    const auto& a = MediaAssets::get();
    VideoDecoder d(a.hd, {.allowHardware = false, .maxWidth = 480, .maxHeight = 480});
    const auto f = d.nextFrame();
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->width, 480);
    EXPECT_EQ(f->height, 270);
    d.setMaxSize(960, 540);
    const auto g = d.nextFrame();
    EXPECT_EQ(g->width, 960);
    EXPECT_EQ(g->height, 540);
}

TEST(VideoDecoderTest, RgbaOutput) {
    const auto& a = MediaAssets::get();
    VideoDecoder d(a.mpeg4WithAudio, {.allowHardware = false, .rgbaOutput = true});
    const auto f = d.nextFrame();
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->format, PixelFormat::Rgba);
    EXPECT_EQ(f->planeCount, 1);
    EXPECT_EQ(f->planes[0].size(), 320u * 240u * 4u);
}

TEST(VideoDecoderTest, HardwarePathFallsBackOrMatchesSoftware) {
    const auto& a = MediaAssets::get();
    VideoDecoder hw(a.h264, {.allowHardware = true});
    VideoDecoder sw(a.h264, {.allowHardware = false});
    std::printf("hwaccel in use: %s\n", hwAccelName(hw.hwAccel()));
    for (const std::int64_t index : {0LL, 10LL, 45LL}) {
        const auto t = framesToTime(index, a.h264Spec.frameRate);
        const auto h = hw.frameAt(t);
        const auto s = sw.frameAt(t);
        ASSERT_NE(h, nullptr);
        ASSERT_NE(s, nullptr);
        EXPECT_EQ(h->pts, s->pts);
        EXPECT_NEAR(meanLuma(*h), meanLuma(*s), 2);
    }
}

TEST(VideoDecoderTest, ThrowsForFilesWithoutVideo) {
    const auto& a = MediaAssets::get();
    EXPECT_THROW(VideoDecoder(a.wav), MediaError);
    EXPECT_THROW(VideoDecoder(a.notMedia), MediaError);
}

TEST(VideoDecoderTest, StillImageDecodes) {
    const auto& a = MediaAssets::get();
    VideoDecoder d(a.png, {.allowHardware = false, .rgbaOutput = true});
    const auto f = d.frameAt(Rational(3));
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->width, 64);
}

TEST(VideoDecoderTest, ScrubLatencyOn1080p) {
    // Non-functional requirement: scrubbing response < 100 ms. Measured on random seeks across
    // GOPs of a 1080p GOP-30 file with software decoding (worst case). Debug builds are ~10x
    // slower, so the hard limit only applies to optimized builds.
    const auto& a = MediaAssets::get();
    VideoDecoder d(a.hd, {.allowHardware = false, .maxWidth = 960, .maxHeight = 540});
    std::mt19937 rng(11);
    double worst = 0;
    double total = 0;
    constexpr int kSeeks = 15;
    for (int i = 0; i < kSeeks; ++i) {
        const auto index = static_cast<std::int64_t>(rng() % 90U);
        const auto start = std::chrono::steady_clock::now();
        const auto f = d.frameAt(framesToTime(index, a.hdSpec.frameRate));
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        ASSERT_NE(f, nullptr);
        worst = std::max(worst, ms);
        total += ms;
    }
    std::printf("1080p scrub: avg %.1f ms, worst %.1f ms\n", total / kSeeks, worst);
#ifdef NDEBUG
    EXPECT_LT(total / kSeeks, 100.0);
#endif
}

} // namespace

