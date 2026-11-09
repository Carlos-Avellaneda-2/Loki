#include "audio/mixer.h"
#include "media/media_probe.h"
#include "model/effect_registry.h"
#include "model/project.h"
#include "model/transitions.h"
#include "playback/sources.h"

#include "media_assets.h"
#include "test_temp_dir.h"

#include <gtest/gtest.h>

#include <fstream>

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

model::Clip clip(const model::MediaItem& media, Rational start, Rational duration, Rational sourceIn = {}) {
    model::Clip c;
    c.id = model::ClipId::generate();
    c.media = media.id;
    c.start = start;
    c.duration = duration;
    c.sourceIn = sourceIn;
    return c;
}

class TransitionSourceTest : public ::testing::Test {
protected:
    model::MediaItem video = mediaItem(MediaAssets::get().mpeg4WithAudio); // 25 fps, 2 s
    model::MediaItem tone = mediaItem(MediaAssets::get().wav);             // 2 s, 0.5 amplitude

    std::shared_ptr<SequenceSnapshot> twoClips(std::string_view type) {
        auto s = std::make_shared<SequenceSnapshot>();
        s->sequence = model::makeDefaultSequence("S", {}, 1, 1);
        s->sequence.settings.frameRate = Rational(25);
        s->sequence.settings.width = 320;
        s->sequence.settings.height = 240;
        s->media[video.id] = video;
        s->media[tone.id] = tone;
        auto& v = s->sequence.videoTracks[0];
        v.clips = {clip(video, Rational(0), Rational(1)), clip(video, Rational(1), Rational(1), Rational(1, 2))};
        v.transitions.push_back({model::TransitionId::generate(), std::string(type), v.clips[0].id, v.clips[1].id,
                                 Rational(2, 5), model::TransitionAlignment::Center});
        s->generation = 1;
        return s;
    }
    render::CompositePtr compositeAt(const std::shared_ptr<SequenceSnapshot>& s, Rational t) {
        SequenceVideoSource src(s, false, 0, 0);
        const auto f = src.frameAt(t);
        return f ? f->composite : nullptr;
    }
};

TEST_F(TransitionSourceTest, CrossDissolveDrawsBothClipsAcrossTheCut) {
    auto s = twoClips(model::kCrossDissolve);
    const auto mid = compositeAt(s, Rational(1)); // span 0.8 .. 1.2: progress 0.5
    ASSERT_NE(mid, nullptr);
    ASSERT_EQ(mid->layers.size(), 2u);
    EXPECT_DOUBLE_EQ(mid->layers[0].transform.opacity, 100.0);
    EXPECT_NEAR(mid->layers[1].transform.opacity, 50.0, 0.01);
    // Before the span: only the outgoing clip.
    EXPECT_EQ(compositeAt(s, Rational(3, 5))->layers.size(), 1u);
    // The outgoing clip plays past its end (handles: source 1.0 .. 1.2 s exists).
    EXPECT_NEAR(mid->layers[0].frame->pts.toDouble(), 1.0, 0.05);
}

TEST_F(TransitionSourceTest, DipToBlackAndWipe) {
    auto dip = twoClips(model::kDipToBlack);
    const auto early = compositeAt(dip, Rational(22, 25)); // progress 0.2: outgoing dims to 40 %
    ASSERT_EQ(early->layers.size(), 1u);
    EXPECT_NEAR(early->layers[0].grade.dim, 0.4f, 0.01f);
    const auto late = compositeAt(dip, Rational(28, 25)); // progress 0.8: incoming at 40 %
    ASSERT_EQ(late->layers.size(), 1u);
    EXPECT_NEAR(late->layers[0].grade.dim, 0.4f, 0.01f);
    auto wipe = twoClips(model::kWipe);
    const auto w = compositeAt(wipe, Rational(1));
    ASSERT_EQ(w->layers.size(), 2u);
    EXPECT_TRUE(w->layers[1].wipe.enabled);
    EXPECT_NEAR(w->layers[1].wipe.progress, 0.5f, 0.01f);
}

TEST_F(TransitionSourceTest, ColorAndLutEffectsReachTheLayer) {
    loki::test::TempDir dir;
    const auto cube = dir.path() / "invert.cube";
    {
        std::ofstream out(cube);
        out << "LUT_3D_SIZE 2\n";
        for (int i = 0; i < 8; ++i) {
            out << (1 - (i & 1)) << ' ' << ((i >> 1) & 1) << ' ' << ((i >> 2) & 1) << '\n';
        }
    }
    auto s = twoClips(model::kCrossDissolve);
    auto& c = s->sequence.videoTracks[0].clips[0];
    auto color = model::makeEffect(model::kColorEffect, video.info, s->sequence.settings);
    color.parameters[0].value = 1.5; // exposure
    c.effects.push_back(color);
    auto lut = model::makeEffect(model::kLutEffect, video.info, s->sequence.settings);
    lut.strings["file"] = cube.string();
    lut.parameters[0].value = 50;
    c.effects.push_back(lut);
    const auto comp = compositeAt(s, Rational(1, 5));
    ASSERT_EQ(comp->layers.size(), 1u);
    EXPECT_FLOAT_EQ(comp->layers[0].grade.color.exposure, 1.5f);
    ASSERT_NE(comp->layers[0].grade.lut, nullptr);
    EXPECT_EQ(comp->layers[0].grade.lut->size, 2);
    EXPECT_FLOAT_EQ(comp->layers[0].grade.lutMix, 0.5f);
    // A missing LUT file is skipped (logged), not fatal.
    s->sequence.videoTracks[0].clips[0].effects[1].strings["file"] = (dir.path() / "missing.cube").string();
    EXPECT_EQ(compositeAt(s, Rational(1, 5))->layers[0].grade.lut, nullptr);
}

TEST_F(TransitionSourceTest, AudioCrossfadeIsConstantPower) {
    auto s = twoClips(model::kCrossDissolve);
    auto& a = s->sequence.audioTracks[0];
    a.clips = {clip(tone, Rational(0), Rational(1)), clip(tone, Rational(1), Rational(1), Rational(1, 2))};
    a.transitions.push_back({model::TransitionId::generate(), std::string(model::kCrossfade), a.clips[0].id,
                             a.clips[1].id, Rational(2, 5), model::TransitionAlignment::Center});
    SequenceAudioSource src(s, 48000, 2);
    std::vector<float> buf(4800 * 2);
    // Inside the crossfade the two equal tones add with cos/sin gains (not in phase: < 0.5 * sqrt2).
    src.seek(Rational(1) - Rational(1, 20));
    src.read(buf.data(), 4800);
    const auto mid = audio::measure(buf.data(), 4800, 2);
    EXPECT_GT(mid.left.rms, 0.2f);
    // At the span start the incoming clip is silent and the outgoing at full level.
    src.seek(Rational(4, 5));
    src.read(buf.data(), 240);
    EXPECT_NEAR(audio::measure(buf.data(), 240, 2).left.peak, 0.5f, 0.05f);
}

} // namespace
