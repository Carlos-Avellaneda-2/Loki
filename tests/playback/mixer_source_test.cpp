#include "audio/mixer.h"
#include "media/media_probe.h"
#include "model/effect_registry.h"
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
using loki::test::MediaAssets;

model::MediaItem mediaItem(const std::filesystem::path& path) {
    model::MediaItem m;
    m.id = model::MediaId::generate();
    m.name = path.stem().string();
    m.path = path;
    m.info = media::probeMedia(path);
    return m;
}

model::Clip clip(const model::MediaItem& media, Rational start, Rational duration) {
    model::Clip c;
    c.id = model::ClipId::generate();
    c.media = media.id;
    c.start = start;
    c.duration = duration;
    return c;
}

model::Effect effect(std::string_view type, std::string key, double value) {
    model::Effect e;
    e.id = model::EffectId::generate();
    e.type = std::string(type);
    e.parameters.push_back({std::move(key), value, {}});
    return e;
}

// Reads [from, from + frames) at 48 kHz stereo and measures it.
audio::StereoLevel levelOf(const std::shared_ptr<const SequenceSnapshot>& s, Rational from, std::size_t frames) {
    SequenceAudioSource src(s, 48000, 2);
    src.seek(from);
    std::vector<float> buf(frames * 2, 0.0f);
    const auto n = src.read(buf.data(), frames);
    return audio::measure(buf.data(), n, 2);
}

class MixerSourceTest : public ::testing::Test {
protected:
    model::MediaItem tone = mediaItem(MediaAssets::get().wav);     // 2 s stereo, 440 Hz, amplitude 0.5
    model::MediaItem mono = mediaItem(MediaAssets::get().monoWav); // 1 s mono

    std::shared_ptr<SequenceSnapshot> snapshot() {
        auto s = std::make_shared<SequenceSnapshot>();
        s->sequence = model::makeDefaultSequence("S", {}, 1, 2);
        s->media[tone.id] = tone;
        s->media[mono.id] = mono;
        s->generation = 1;
        return s;
    }
};

TEST_F(MixerSourceTest, UnityMixKeepsTheSource) {
    auto s = snapshot();
    s->sequence.audioTracks[0].clips.push_back(clip(tone, Rational(0), Rational(1)));
    const auto level = levelOf(s, Rational(1, 4), 4800);
    EXPECT_NEAR(level.left.peak, 0.5f, 0.02f);
    EXPECT_NEAR(level.left.rms, 0.5f / std::sqrt(2.0f), 0.02f);
}

TEST_F(MixerSourceTest, ClipTrackAndMasterGainsMultiply) {
    auto s = snapshot();
    auto c = clip(tone, Rational(0), Rational(1));
    c.effects.push_back(effect(model::kVolumeEffect, "level", -6.0206));
    s->sequence.audioTracks[0].clips.push_back(c);
    s->sequence.audioTracks[0].state.volumeDb = -6.0206;
    s->sequence.masterVolumeDb = -6.0206;
    EXPECT_NEAR(levelOf(s, Rational(1, 4), 4800).right.peak, 0.0625f, 0.004f);
}

TEST_F(MixerSourceTest, BalanceHardLeftSilencesTheRight) {
    auto s = snapshot();
    s->sequence.audioTracks[0].clips.push_back(clip(tone, Rational(0), Rational(1)));
    s->sequence.audioTracks[0].state.pan = -100;
    const auto level = levelOf(s, Rational(1, 4), 4800);
    EXPECT_NEAR(level.left.peak, 0.5f, 0.02f);
    EXPECT_LT(level.right.peak, 1e-4f);
}

TEST_F(MixerSourceTest, MonoClipPanIsConstantPower) {
    auto s = snapshot();
    auto c = clip(mono, Rational(0), Rational(1));
    s->sequence.audioTracks[0].clips.push_back(c);
    const auto center = levelOf(s, Rational(1, 4), 4800);
    s->sequence.audioTracks[0].clips[0].effects.push_back(effect(model::kPannerEffect, "pan", -100));
    const auto left = levelOf(s, Rational(1, 4), 4800);
    ASSERT_GT(center.left.peak, 0.05f);
    // Center: -3 dB per side; hard left: the full mono signal on the left only.
    EXPECT_NEAR(left.left.peak / center.left.peak, std::sqrt(2.0f), 0.05f);
    EXPECT_LT(left.right.peak, 1e-4f);
}

TEST_F(MixerSourceTest, FadeInStartsSilentAndReachesFullLevel) {
    auto s = snapshot();
    auto c = clip(tone, Rational(0), Rational(1));
    c.fadeIn = Rational(1, 2);
    s->sequence.audioTracks[0].clips.push_back(c);
    EXPECT_LT(levelOf(s, Rational(0), 240).left.peak, 0.02f);                 // first 5 ms
    EXPECT_NEAR(levelOf(s, Rational(1, 4), 480).left.peak, 0.5f * 0.707f, 0.03f); // half way: sin(pi/4)
    EXPECT_NEAR(levelOf(s, Rational(3, 5), 4800).left.peak, 0.5f, 0.02f);
}

TEST_F(MixerSourceTest, SoloAndMuteSelectTracksAndLevelsAreReported) {
    auto s = snapshot();
    s->sequence.audioTracks[0].clips.push_back(clip(tone, Rational(0), Rational(1)));
    s->sequence.audioTracks[1].clips.push_back(clip(tone, Rational(0), Rational(1)));
    s->sequence.audioTracks[1].state.volumeDb = -6.0206;
    EXPECT_NEAR(levelOf(s, Rational(1, 4), 4800).left.peak, 0.75f, 0.03f);
    s->sequence.audioTracks[1].state.solo = true;
    {
        SequenceAudioSource src(s, 48000, 2);
        src.seek(Rational(1, 4));
        std::vector<float> buf(4800 * 2);
        src.read(buf.data(), 4800);
        EXPECT_NEAR(audio::measure(buf.data(), 4800, 2).left.peak, 0.25f, 0.02f);
        const auto levels = src.trackLevels();
        ASSERT_EQ(levels.size(), 2u);
        EXPECT_EQ(levels[0].track, s->sequence.audioTracks[0].id);
        EXPECT_EQ(levels[0].level.left.peak, 0.0f); // not soloed: silent
        EXPECT_NEAR(levels[1].level.left.peak, 0.25f, 0.02f);
    }
    s->sequence.audioTracks[1].state.muted = true; // mute wins over solo
    EXPECT_LT(levelOf(s, Rational(1, 4), 4800).left.peak, 1e-4f);
}

TEST_F(MixerSourceTest, FaderChangesRampWithoutClicks) {
    auto s = snapshot();
    s->sequence.audioTracks[0].clips.push_back(clip(tone, Rational(0), Rational(2)));
    SequenceAudioSource src(s, 48000, 2);
    std::vector<float> buf(1024 * 2);
    src.read(buf.data(), 1024);
    auto quiet = std::make_shared<SequenceSnapshot>(*s);
    quiet->sequence.audioTracks[0].state.volumeDb = audio::kSilenceDb;
    ASSERT_TRUE(sameExceptAudioMix(*s, *quiet));
    src.updateMix(quiet);
    src.read(buf.data(), 1024);
    // The block fades from full level to silence instead of jumping.
    EXPECT_GT(audio::measure(buf.data(), 64, 2).left.peak, 0.3f);
    EXPECT_LT(audio::measure(buf.data() + 960 * 2, 64, 2).left.peak, 0.05f);
    src.read(buf.data(), 1024);
    EXPECT_EQ(audio::measure(buf.data(), 1024, 2).left.peak, 0.0f);
}

TEST_F(MixerSourceTest, SameExceptAudioMixDetectsStructuralEdits) {
    auto s = snapshot();
    s->sequence.audioTracks[0].clips.push_back(clip(tone, Rational(0), Rational(1)));
    auto mix = std::make_shared<SequenceSnapshot>(*s);
    mix->sequence.masterVolumeDb = -3;
    mix->sequence.audioTracks[0].state.pan = 20;
    mix->sequence.audioTracks[0].clips[0].fadeOut = Rational(1, 4);
    mix->sequence.audioTracks[0].clips[0].effects.push_back(effect(model::kVolumeEffect, "level", -2));
    EXPECT_TRUE(sameExceptAudioMix(*s, *mix));
    auto moved = std::make_shared<SequenceSnapshot>(*s);
    moved->sequence.audioTracks[0].clips[0].start = Rational(1, 2);
    EXPECT_FALSE(sameExceptAudioMix(*s, *moved));
    auto hidden = std::make_shared<SequenceSnapshot>(*s);
    hidden->sequence.videoTracks[0].state.hidden = true;
    EXPECT_FALSE(sameExceptAudioMix(*s, *hidden));
}

TEST_F(MixerSourceTest, EngineAppliesMixEditsWithoutReloadingAndReportsLevels) {
    auto s = snapshot();
    s->sequence.audioTracks[0].clips.push_back(clip(tone, Rational(0), Rational(2)));
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
    EXPECT_FALSE(engine.levels().active);
    engine.play();
    ASSERT_TRUE(engine.waitIdle());
    device->pump(4800);
    const auto levels = engine.levels();
    ASSERT_TRUE(levels.active);
    EXPECT_NEAR(levels.master.left.peak, 0.5f, 0.03f);
    ASSERT_EQ(levels.tracks.size(), 2u);
    EXPECT_NEAR(levels.tracks[0].level.left.peak, 0.5f, 0.03f);

    const auto opens = engine.openCount();
    auto quieter = std::make_shared<SequenceSnapshot>(*s);
    quieter->generation = 2;
    quieter->sequence.audioTracks[0].state.volumeDb = -12;
    engine.openSequence(quieter);
    ASSERT_TRUE(engine.waitIdle());
    EXPECT_EQ(engine.openCount(), opens); // decoders and buffers kept
    EXPECT_TRUE(engine.isPlaying());
    engine.pause();
    ASSERT_TRUE(engine.waitIdle());
    EXPECT_FALSE(engine.levels().active);
}

} // namespace
