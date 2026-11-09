#include "media/media_probe.h"
#include "model/project.h"
#include "playback/playback_engine.h"
#include "playback/sources.h"

#include "media_assets.h"

#include <gtest/gtest.h>

#include <cmath>
#include <thread>

namespace {

using namespace loki;
using namespace loki::playback;
using loki::test::expectedLuma;
using loki::test::MediaAssets;

model::MediaItem mediaItem(const std::filesystem::path& path) {
    model::MediaItem m;
    m.id = model::MediaId::generate();
    m.name = path.stem().string();
    m.path = path;
    m.info = media::probeMedia(path);
    return m;
}

model::Clip clip(const model::MediaItem& media, Rational start, Rational duration, Rational sourceIn = {}) {
    model::Clip c;
    c.id = model::ClipId::generate();
    c.media = media.id;
    c.start = start;
    c.duration = duration;
    c.sourceIn = sourceIn;
    return c;
}

int luma(const media::VideoFramePtr& f) {
    return f ? f->planes[0][f->planes[0].size() / 2] : -1;
}

class SequenceSourceTest : public ::testing::Test {
protected:
    model::MediaItem video = mediaItem(MediaAssets::get().mpeg4WithAudio); // 25 fps, 2 s
    model::MediaItem tone = mediaItem(MediaAssets::get().wav);             // 2 s, 440 Hz, 0.5

    std::shared_ptr<SequenceSnapshot> snapshot() {
        auto s = std::make_shared<SequenceSnapshot>();
        s->sequence = model::makeDefaultSequence("S", {}, 2, 2);
        s->sequence.settings.frameRate = Rational(25);
        s->media[video.id] = video;
        s->media[tone.id] = tone;
        s->generation = 1;
        return s;
    }
};

TEST_F(SequenceSourceTest, MapsSequenceTimeToClipSourceTime) {
    auto s = snapshot();
    // Clip on V1 at 1 s showing source frames from 0.4 s (frame 10).
    s->sequence.videoTracks[0].clips.push_back(clip(video, Rational(1), Rational(1), Rational(10, 25)));
    SequenceVideoSource src(s, false, 0, 0);
    const auto f = src.frameAt(Rational(1) + Rational(5, 25)); // 5 frames into the clip
    ASSERT_TRUE(f.has_value());
    EXPECT_EQ(f->time, Rational(30, 25));
    EXPECT_EQ(f->duration, Rational(1, 25));
    EXPECT_NEAR(luma(f->frame), expectedLuma(15), 3);
}

TEST_F(SequenceSourceTest, GapsAreBlackAndEndIsNullopt) {
    auto s = snapshot();
    s->sequence.videoTracks[0].clips.push_back(clip(video, Rational(1), Rational(1)));
    SequenceVideoSource src(s, false, 0, 0);
    const auto gap = src.frameAt(Rational(1, 2));
    ASSERT_TRUE(gap.has_value());
    EXPECT_EQ(gap->frame, nullptr);
    EXPECT_FALSE(src.frameAt(Rational(2)).has_value());
}

TEST_F(SequenceSourceTest, TopmostVisibleTrackWins) {
    auto s = snapshot();
    s->sequence.videoTracks[0].clips.push_back(clip(video, Rational(0), Rational(1), Rational(0)));
    s->sequence.videoTracks[1].clips.push_back(clip(video, Rational(0), Rational(1), Rational(20, 25)));
    {
        SequenceVideoSource src(s, false, 0, 0);
        EXPECT_NEAR(luma(src.frameAt(Rational())->frame), expectedLuma(20), 3); // V2 over V1
    }
    s->sequence.videoTracks[1].state.hidden = true;
    SequenceVideoSource src(s, false, 0, 0);
    EXPECT_NEAR(luma(src.frameAt(Rational())->frame), expectedLuma(0), 3);
}

TEST_F(SequenceSourceTest, SequentialPlaybackAcrossClips) {
    auto s = snapshot();
    s->sequence.videoTracks[0].clips.push_back(clip(video, Rational(0), Rational(10, 25), Rational(30, 25)));
    s->sequence.videoTracks[0].clips.push_back(clip(video, Rational(10, 25), Rational(10, 25), Rational(0)));
    SequenceVideoSource src(s, false, 0, 0);
    src.seek(Rational(8, 25));
    const int expected[] = {38, 39, 0, 1, 2};
    for (const int e : expected) {
        const auto f = src.nextFrame();
        ASSERT_TRUE(f.has_value());
        EXPECT_NEAR(luma(f->frame), expectedLuma(e), 3);
    }
}

TEST_F(SequenceSourceTest, AudioSumsUnmutedTracks) {
    auto s = snapshot();
    s->sequence.audioTracks[0].clips.push_back(clip(tone, Rational(0), Rational(1)));
    s->sequence.audioTracks[1].clips.push_back(clip(tone, Rational(1, 2), Rational(1)));
    {
        SequenceAudioSource src(s, 48000, 2);
        std::vector<float> buf(48000 * 2 * 2, 0.0f);
        const auto n = src.read(buf.data(), 48000 * 2);
        EXPECT_EQ(n, 72000u); // sequence ends at 1.5 s
        float peakSingle = 0;
        float peakDouble = 0;
        for (std::size_t i = 0; i < 24000; ++i) {
            peakSingle = std::max(peakSingle, std::abs(buf[i * 2]));
        }
        for (std::size_t i = 24000; i < 48000; ++i) {
            peakDouble = std::max(peakDouble, std::abs(buf[i * 2]));
        }
        EXPECT_NEAR(peakSingle, 0.5f, 0.01f);
        EXPECT_NEAR(peakDouble, 1.0f, 0.01f); // in phase: 0.5 + 0.5 (clamped at 1)
        EXPECT_EQ(src.read(buf.data(), 100), 0u);
    }
    s->sequence.audioTracks[1].state.muted = true;
    SequenceAudioSource src(s, 48000, 2);
    std::vector<float> buf(48000 * 2, 0.0f);
    src.seek(Rational(1, 2));
    src.read(buf.data(), 24000);
    float peak = 0;
    for (const float v : buf) {
        peak = std::max(peak, std::abs(v));
    }
    EXPECT_NEAR(peak, 0.5f, 0.01f);
}

TEST_F(SequenceSourceTest, EnginePlaysSequenceInSyncWithAudio) {
    auto s = snapshot();
    s->sequence.videoTracks[0].clips.push_back(clip(video, Rational(0), Rational(1)));
    s->sequence.audioTracks[0].clips.push_back(clip(tone, Rational(0), Rational(1)));
    audio::ManualAudioDevice* device = nullptr;
    PlaybackOptions options;
    options.allowHardware = false;
    options.deviceFactory = [&](audio::RenderCallback cb) {
        auto d = std::make_unique<audio::ManualAudioDevice>(std::move(cb));
        device = d.get();
        return d;
    };
    PlaybackEngine engine(std::move(options));
    engine.openSequence(s);
    ASSERT_TRUE(engine.waitIdle());
    EXPECT_TRUE(engine.isOpen());
    EXPECT_EQ(engine.duration(), Rational(1));
    EXPECT_EQ(engine.frameRate(), Rational(25));
    engine.play();
    ASSERT_TRUE(engine.waitIdle());
    for (int i = 1; i <= 10; ++i) {
        device->pump(1920);
        std::int64_t shown = -1;
        for (int k = 0; k < 400 && shown != i; ++k) {
            (void)engine.currentFrame();
            shown = timeToFrames(engine.currentFrameTime(), Rational(25), Rounding::Nearest);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        EXPECT_EQ(shown, i);
    }
    // Live update: a new snapshot keeps the position.
    engine.pause();
    ASSERT_TRUE(engine.waitIdle());
    const auto pos = engine.position();
    auto edited = std::make_shared<SequenceSnapshot>(*s);
    edited->generation = 2;
    edited->sequence.videoTracks[0].clips[0].sourceIn = Rational(20, 25);
    engine.openSequence(edited);
    ASSERT_TRUE(engine.waitIdle());
    EXPECT_EQ(engine.position(), pos);
    EXPECT_NEAR(luma(engine.currentFrame()), expectedLuma(20 + timeToFrames(pos, Rational(25), Rounding::Floor)), 3);
    // Seeking past the end is allowed (free playhead) and shows black.
    engine.seek(Rational(5));
    ASSERT_TRUE(engine.waitIdle());
    EXPECT_EQ(engine.position(), Rational(5));
    EXPECT_EQ(engine.currentFrame(), nullptr);
}

} // namespace
