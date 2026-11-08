#include "media/media_probe.h"
#include "playback/playback_clock.h"
#include "playback/playback_engine.h"
#include "render/color_conversion.h"

#include "media_assets.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>

namespace {

using namespace loki;
using namespace loki::playback;
using loki::test::expectedLuma;
using loki::test::MediaAssets;
using namespace std::chrono_literals;

// Deterministic time for wall-clock playback.
struct FakeTime {
    std::atomic<std::int64_t> us{1'000'000};
    std::chrono::steady_clock::time_point now() const {
        return std::chrono::steady_clock::time_point(std::chrono::microseconds(us.load()));
    }
    void advance(std::chrono::microseconds d) { us += d.count(); }
};

// --- Clock & shuttle -------------------------------------------------------------------------

TEST(PlaybackClockTest, WallClockWithRate) {
    FakeTime time;
    PlaybackClock clock([&] { return time.now(); });
    clock.start(Rational(10), 2, false);
    time.advance(500ms);
    EXPECT_EQ(clock.now(), Rational(11));
    clock.pause();
    time.advance(1s);
    EXPECT_EQ(clock.now(), Rational(11));
    EXPECT_FALSE(clock.isRunning());
    clock.start(Rational(11), -1, false);
    time.advance(250ms);
    EXPECT_EQ(clock.now(), Rational(43, 4));
}

TEST(PlaybackClockTest, AudioDrivenFollowsConsumedFramesMinusLatency) {
    FakeTime time;
    PlaybackClock clock([&] { return time.now(); });
    clock.start(Rational(2), 1, true, 48000, 960);
    EXPECT_TRUE(clock.isAudioDriven());
    time.advance(10s); // wall time is irrelevant when audio drives
    EXPECT_EQ(clock.now(), Rational(2));
    clock.onAudioFramesPlayed(480);
    EXPECT_EQ(clock.now(), Rational(2)); // still inside the device latency
    clock.onAudioFramesPlayed(48000 + 480);
    EXPECT_EQ(clock.now(), Rational(3));
}

TEST(PlaybackClockTest, AudioDrivenOnlyAtNormalSpeed) {
    PlaybackClock clock;
    clock.start(Rational(), 2, true);
    EXPECT_FALSE(clock.isAudioDriven());
}

TEST(PlaybackClockTest, SetPositionWhilePaused) {
    PlaybackClock clock;
    clock.setPosition(Rational(7, 2));
    EXPECT_EQ(clock.now(), Rational(7, 2));
}

TEST(ShuttleTest, PremiereJklBehaviour) {
    EXPECT_EQ(nextShuttleRate(0, ShuttleKey::L), 1);
    EXPECT_EQ(nextShuttleRate(1, ShuttleKey::L), 2);
    EXPECT_EQ(nextShuttleRate(2, ShuttleKey::L), 4);
    EXPECT_EQ(nextShuttleRate(4, ShuttleKey::L), 8);
    EXPECT_EQ(nextShuttleRate(8, ShuttleKey::L), 8);
    EXPECT_EQ(nextShuttleRate(-4, ShuttleKey::L), 1);
    EXPECT_EQ(nextShuttleRate(0, ShuttleKey::J), -1);
    EXPECT_EQ(nextShuttleRate(-1, ShuttleKey::J), -2);
    EXPECT_EQ(nextShuttleRate(-8, ShuttleKey::J), -8);
    EXPECT_EQ(nextShuttleRate(4, ShuttleKey::J), -1);
    EXPECT_EQ(nextShuttleRate(4, ShuttleKey::K), 0);
}

// --- Color conversion ------------------------------------------------------------------------

TEST(ColorConversionTest, LimitedRangeBlackWhiteGray) {
    for (const auto m : {media::ColorMatrix::Bt601, media::ColorMatrix::Bt709}) {
        const auto c = render::yuvToRgb(m, media::ColorRange::Limited);
        const auto black = render::convertPixel(c, 16 / 255.0f, 128 / 255.0f, 128 / 255.0f);
        const auto white = render::convertPixel(c, 235 / 255.0f, 128 / 255.0f, 128 / 255.0f);
        for (int i = 0; i < 3; ++i) {
            EXPECT_NEAR(black[static_cast<std::size_t>(i)], 0.0f, 1e-4);
            EXPECT_NEAR(white[static_cast<std::size_t>(i)], 1.0f, 1e-4);
        }
    }
}

TEST(ColorConversionTest, Bt709PrimaryRed) {
    // BT.709 limited-range 8-bit red: Y=63, Cb=102, Cr=240.
    const auto c = render::yuvToRgb(media::ColorMatrix::Bt709, media::ColorRange::Limited);
    const auto rgb = render::convertPixel(c, 63 / 255.0f, 102 / 255.0f, 240 / 255.0f);
    EXPECT_NEAR(rgb[0], 1.0f, 0.01f);
    EXPECT_NEAR(rgb[1], 0.0f, 0.01f);
    EXPECT_NEAR(rgb[2], 0.0f, 0.01f);
}

TEST(ColorConversionTest, FullRangeAndFitRect) {
    const auto c = render::yuvToRgb(media::ColorMatrix::Bt601, media::ColorRange::Full);
    const auto gray = render::convertPixel(c, 0.5f, 128 / 255.0f, 128 / 255.0f);
    EXPECT_NEAR(gray[1], 0.5f, 1e-4);
    const auto r = render::fitRect(1920, 1080, 1000, 1000);
    EXPECT_NEAR(r[0], 0.0f, 1e-3);
    EXPECT_NEAR(r[2], 1000.0f, 1e-3);
    EXPECT_NEAR(r[3], 562.5f, 1e-3);
    EXPECT_NEAR(r[1], (1000.0f - 562.5f) / 2.0f, 1e-3);
}

// --- Engine ----------------------------------------------------------------------------------

class PlaybackEngineTest : public ::testing::Test {
protected:
    FakeTime time;
    audio::ManualAudioDevice* device = nullptr;
    std::unique_ptr<PlaybackEngine> engine;

    void SetUp() override {
        PlaybackOptions options;
        options.allowHardware = false;
        options.timeSource = [this] { return time.now(); };
        options.deviceFactory = [this](audio::RenderCallback cb) {
            auto d = std::make_unique<audio::ManualAudioDevice>(std::move(cb), 48000, 2);
            device = d.get();
            return d;
        };
        engine = std::make_unique<PlaybackEngine>(std::move(options));
    }

    void open(const std::filesystem::path& path) {
        const auto before = engine->openCount();
        engine->open(path, media::probeMedia(path));
        ASSERT_TRUE(engine->waitIdle());
        ASSERT_GT(engine->openCount(), before);
    }

    static std::int64_t indexOf(const media::VideoFramePtr& f, const Rational& rate) {
        return f ? timeToFrames(f->pts, rate, Rounding::Nearest) : -1;
    }

    // Polls until the displayed frame is the one for the current clock (the media thread fills
    // its queue asynchronously). Returns the frame index shown.
    std::int64_t settle(const Rational& rate) {
        std::int64_t shown = -1;
        for (int i = 0; i < 400; ++i) {
            const auto expected = timeToFrames(engine->position(), rate, Rounding::Floor);
            shown = indexOf(engine->currentFrame(), rate);
            if (shown == expected) {
                return shown;
            }
            std::this_thread::sleep_for(5ms);
        }
        return shown;
    }
};

TEST_F(PlaybackEngineTest, OpenShowsFirstFrame) {
    const auto& a = MediaAssets::get();
    open(a.mpeg4WithAudio);
    EXPECT_TRUE(engine->isOpen());
    EXPECT_EQ(engine->frameRate(), Rational(25));
    const auto f = engine->currentFrame();
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->pts, Rational());
    EXPECT_FALSE(engine->isPlaying());
}

TEST_F(PlaybackEngineTest, OpenFailureReportsError) {
    const auto& a = MediaAssets::get();
    model::MediaInfo info;
    info.hasVideo = true;
    engine->open(a.notMedia, info);
    ASSERT_TRUE(engine->waitIdle());
    EXPECT_FALSE(engine->isOpen());
    EXPECT_FALSE(engine->lastError().empty());
}

TEST_F(PlaybackEngineTest, ScrubLatestRequestWins) {
    const auto& a = MediaAssets::get();
    open(a.mpeg4WithAudio);
    for (int i = 0; i < 30; ++i) {
        engine->seek(Rational(i, 25));
    }
    engine->seek(Rational(37, 25));
    ASSERT_TRUE(engine->waitIdle());
    EXPECT_EQ(indexOf(engine->currentFrame(), Rational(25)), 37);
    EXPECT_EQ(engine->position(), Rational(37, 25));
    EXPECT_GE(engine->stats().lastSeekMs, 0.0);
}

TEST_F(PlaybackEngineTest, StepFrames) {
    const auto& a = MediaAssets::get();
    open(a.mpeg4WithAudio);
    engine->stepFrames(3);
    ASSERT_TRUE(engine->waitIdle());
    EXPECT_EQ(indexOf(engine->currentFrame(), Rational(25)), 3);
    engine->stepFrames(-1);
    ASSERT_TRUE(engine->waitIdle());
    EXPECT_EQ(indexOf(engine->currentFrame(), Rational(25)), 2);
    engine->stepFrames(-10); // clamps at the start
    ASSERT_TRUE(engine->waitIdle());
    EXPECT_EQ(indexOf(engine->currentFrame(), Rational(25)), 0);
}

TEST_F(PlaybackEngineTest, AudioIsMasterClockAndVideoStaysInSync) {
    const auto& a = MediaAssets::get();
    open(a.mpeg4WithAudio);
    engine->play();
    ASSERT_TRUE(engine->waitIdle());
    const Rational rate(25);
    double maxAbsSample = 0;
    for (int i = 0; i < 30; ++i) {
        // One video frame of audio (48000 / 25 = 1920 frames), consumed like a device would.
        const auto samples = device->pump(1920);
        for (const float s : samples) {
            maxAbsSample = std::max(maxAbsSample, static_cast<double>(std::abs(s)));
        }
        EXPECT_EQ(engine->position(), Rational(i + 1, 25)); // clock == audio consumed
        const auto shown = settle(rate);
        // A/V sync requirement: less than one frame of error.
        EXPECT_EQ(shown, i + 1) << "video frame shown at audio time " << (i + 1) << "/25";
        ASSERT_TRUE(engine->currentFrame());
        EXPECT_NEAR(static_cast<int>(engine->currentFrame()->planes[0][100]), expectedLuma(shown), 3);
    }
    EXPECT_NEAR(maxAbsSample, 0.5, 0.06); // the 440 Hz test tone reached the device
    // Time does not advance without audio consumption, even if wall time passes.
    time.advance(2s);
    EXPECT_EQ(engine->position(), Rational(30, 25));
    engine->pause();
    ASSERT_TRUE(engine->waitIdle());
    EXPECT_FALSE(engine->isPlaying());
    EXPECT_EQ(engine->position(), Rational(30, 25));
}

TEST_F(PlaybackEngineTest, ShuttleForwardUsesWallClock) {
    const auto& a = MediaAssets::get();
    open(a.h264); // no audio
    engine->shuttle(ShuttleKey::L);
    engine->shuttle(ShuttleKey::L); // 2x
    EXPECT_EQ(engine->rate(), 2);
    ASSERT_TRUE(engine->waitIdle());
    time.advance(500ms);
    EXPECT_EQ(engine->position(), Rational(1));
    const auto rate = a.h264Spec.frameRate;
    EXPECT_EQ(settle(rate), timeToFrames(Rational(1), rate, Rounding::Floor));
    engine->shuttle(ShuttleKey::K);
    ASSERT_TRUE(engine->waitIdle());
    EXPECT_EQ(engine->rate(), 0);
}

TEST_F(PlaybackEngineTest, ReversePlayback) {
    const auto& a = MediaAssets::get();
    const auto rate = a.h264Spec.frameRate;
    open(a.h264);
    engine->seek(framesToTime(40, rate));
    ASSERT_TRUE(engine->waitIdle());
    engine->shuttle(ShuttleKey::J);
    ASSERT_TRUE(engine->waitIdle());
    for (int step = 0; step < 6; ++step) {
        time.advance(std::chrono::microseconds(1'000'000 * 1001 / 30000 * 3)); // ~3 frames back
        const auto shown = settle(rate);
        const auto expected = timeToFrames(engine->position(), rate, Rounding::Floor);
        EXPECT_EQ(shown, expected);
        EXPECT_LT(shown, 40);
    }
}

TEST_F(PlaybackEngineTest, StopsAtEnd) {
    const auto& a = MediaAssets::get();
    open(a.h264);
    engine->setRate(8);
    ASSERT_TRUE(engine->waitIdle());
    time.advance(5s); // far past the 2 s clip
    for (int i = 0; i < 200 && engine->isPlaying(); ++i) {
        std::this_thread::sleep_for(5ms);
    }
    ASSERT_TRUE(engine->waitIdle());
    EXPECT_FALSE(engine->isPlaying());
    const auto last = engine->duration() - a.h264Spec.frameRate.reciprocal();
    EXPECT_EQ(engine->position(), last);
}

TEST_F(PlaybackEngineTest, AudioOnlyMedia) {
    const auto& a = MediaAssets::get();
    open(a.wav);
    EXPECT_TRUE(engine->isOpen());
    EXPECT_EQ(engine->currentFrame(), nullptr);
    engine->play();
    ASSERT_TRUE(engine->waitIdle());
    const auto samples = device->pump(4800);
    EXPECT_EQ(engine->position(), Rational(1, 10));
    double peak = 0;
    for (const float s : samples) {
        peak = std::max(peak, static_cast<double>(std::abs(s)));
    }
    EXPECT_NEAR(peak, 0.5, 0.01);
}

TEST_F(PlaybackEngineTest, QualityChangesOutputSize) {
    const auto& a = MediaAssets::get();
    open(a.hd);
    engine->setQuality(480, 270);
    ASSERT_TRUE(engine->waitIdle());
    ASSERT_NE(engine->currentFrame(), nullptr);
    EXPECT_EQ(engine->currentFrame()->width, 480);
    engine->setQuality(0, 0);
    ASSERT_TRUE(engine->waitIdle());
    EXPECT_EQ(engine->currentFrame()->width, 1920);
}

TEST_F(PlaybackEngineTest, CloseResets) {
    const auto& a = MediaAssets::get();
    open(a.mpeg4WithAudio);
    engine->close();
    ASSERT_TRUE(engine->waitIdle());
    EXPECT_FALSE(engine->isOpen());
    EXPECT_EQ(engine->currentFrame(), nullptr);
}

} // namespace
