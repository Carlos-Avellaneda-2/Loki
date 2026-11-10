// Phase 15 playback: track automation (D39), clip audio effects and Maintain Pitch (D38), multicam
// angles and grid (D43), render preview keys (D42).
#include "audio/mixer.h"
#include "media/media_probe.h"
#include "model/effect_registry.h"
#include "playback/render_previews.h"
#include "playback/sources.h"

#include "media_assets.h"

#include <gtest/gtest.h>

#include <cmath>

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

double crossingsPerSecond(const std::vector<float>& buf, std::size_t frames) {
    int n = 0;
    for (std::size_t i = 1; i < frames; ++i) {
        if ((buf[(i - 1) * 2] < 0.0f) != (buf[i * 2] < 0.0f)) {
            ++n;
        }
    }
    return n * 48000.0 / static_cast<double>(frames);
}

class Phase15SourceTest : public ::testing::Test {
protected:
    model::MediaItem video = mediaItem(MediaAssets::get().mpeg4WithAudio); // 25 fps, 2 s
    model::MediaItem tone = mediaItem(MediaAssets::get().wav);             // 2 s, 440 Hz, 0.5

    std::shared_ptr<SequenceSnapshot> snapshot() {
        auto s = std::make_shared<SequenceSnapshot>();
        s->sequence = model::makeDefaultSequence("S", {}, 2, 1);
        s->sequence.settings.frameRate = Rational(25);
        s->media[video.id] = video;
        s->media[tone.id] = tone;
        s->generation = 1;
        return s;
    }
    std::vector<float> readAudio(const std::shared_ptr<const SequenceSnapshot>& s, Rational from, std::size_t frames) {
        SequenceAudioSource src(s, 48000, 2);
        src.seek(from);
        std::vector<float> buf(frames * 2, 0.0f);
        std::size_t done = 0;
        while (done < frames) {
            const auto n = src.read(buf.data() + done * 2, std::min<std::size_t>(512, frames - done));
            if (n == 0) {
                break;
            }
            done += n;
        }
        return buf;
    }
    // Peak after `skipFrames` (effects settle: compressor attack, filter start).
    static float peak(const std::vector<float>& buf, std::size_t skipFrames = 0) {
        float p = 0.0f;
        for (std::size_t i = skipFrames * 2; i < buf.size(); ++i) {
            p = std::max(p, std::abs(buf[i]));
        }
        return p;
    }
};

TEST_F(Phase15SourceTest, TrackAutomationRampsTheFader) {
    auto s = snapshot();
    s->sequence.audioTracks[0].clips.push_back(clip(tone, Rational(0), Rational(2)));
    s->sequence.audioTracks[0].state.volumeAutomation = {{Rational(0), 0.0}, {Rational(2), -40.0}};
    EXPECT_NEAR(peak(readAudio(s, Rational(0), 2400)), 0.5f, 0.03f);
    EXPECT_NEAR(peak(readAudio(s, Rational(1), 2400)), 0.5f * audio::dbToGain(-20.0), 0.01f);
    // Mix-only: automation edits do not need a reload.
    auto other = std::make_shared<SequenceSnapshot>(*s);
    other->sequence.audioTracks[0].state.volumeAutomation.clear();
    EXPECT_TRUE(sameExceptAudioMix(*s, *other));
}

TEST_F(Phase15SourceTest, ClipAudioEffectsProcessTheClip) {
    auto s = snapshot();
    auto c = clip(tone, Rational(0), Rational(2));
    auto eq = model::makeEffect(model::kEqEffect, tone.info, s->sequence.settings);
    for (auto& p : eq.parameters) {
        if (p.key == "lowGain") {
            p.value = -24.0; // 440 Hz sits in the low shelf's slope
        }
        if (p.key == "lowFreq") {
            p.value = 1000.0;
        }
    }
    c.effects.push_back(eq);
    s->sequence.audioTracks[0].clips.push_back(c);
    EXPECT_LT(peak(readAudio(s, Rational(1, 2), 4800), 1200), 0.25f);

    auto compressed = snapshot();
    auto c2 = clip(tone, Rational(0), Rational(2));
    auto comp = model::makeEffect(model::kCompressorEffect, tone.info, compressed->sequence.settings);
    for (auto& p : comp.parameters) {
        if (p.key == "threshold") {
            p.value = -30.0;
        }
        if (p.key == "ratio") {
            p.value = 10.0;
        }
    }
    c2.effects.push_back(comp);
    compressed->sequence.audioTracks[0].clips.push_back(c2);
    EXPECT_LT(peak(readAudio(compressed, Rational(1, 2), 4800), 2400), 0.1f);

    auto denoised = snapshot();
    auto c3 = clip(tone, Rational(0), Rational(2));
    c3.effects.push_back(model::makeEffect(model::kNoiseReductionEffect, tone.info, denoised->sequence.settings));
    denoised->sequence.audioTracks[0].clips.push_back(c3);
    const auto out = readAudio(denoised, Rational(1, 2), 4800);
    // A steady tone from the first frame looks like noise to the estimator (it is attenuated, not
    // removed); it still plays, in time (the preroll hides the latency).
    EXPECT_GT(peak(out), 0.04f);
    EXPECT_NEAR(crossingsPerSecond(out, 4800), 880.0, 40.0);
}

TEST_F(Phase15SourceTest, MaintainPitchKeepsTheToneAtDoubleSpeed) {
    auto s = snapshot();
    auto c = clip(tone, Rational(0), Rational(1));
    c.speed = Rational(2);
    c.maintainPitch = true;
    s->sequence.audioTracks[0].clips.push_back(c);
    const auto out = readAudio(s, Rational(1, 5), 9600);
    EXPECT_NEAR(crossingsPerSecond(out, 9600), 880.0, 60.0); // 440 Hz kept (not 880 Hz)
    EXPECT_GT(peak(out), 0.3f);
}

TEST_F(Phase15SourceTest, MulticamClipShowsItsAngleAndTheGridShowsAll) {
    auto inner = snapshot();
    inner->sequence.videoTracks[0].clips.push_back(clip(video, Rational(0), Rational(2), Rational(0)));
    inner->sequence.videoTracks[1].clips.push_back(clip(video, Rational(0), Rational(2), Rational(1, 2)));
    model::MediaItem multicam;
    multicam.id = model::MediaId::generate();
    multicam.name = "MC";
    multicam.generator = std::string(model::kNestGenerator);
    multicam.sequence = inner->sequence.id;
    multicam.multicam = true;
    multicam.info.hasVideo = true;
    auto outer = snapshot();
    outer->media[multicam.id] = multicam;
    outer->nested[inner->sequence.id] = inner;
    auto c = clip(multicam, Rational(0), Rational(2));
    c.angle = 1;
    outer->sequence.videoTracks[0].clips.push_back(c);
    {
        SequenceVideoSource src(outer, false, 0, 0);
        const auto f = src.frameAt(Rational(5, 25));
        ASSERT_TRUE(f.has_value() && f->composite);
        const auto& nested = f->composite->layers.at(0).nested;
        ASSERT_TRUE(nested);
        ASSERT_EQ(nested->layers.size(), 1u); // only camera 2
        EXPECT_NEAR(luma(nested->layers[0].frame), expectedLuma(5 + 12), 3); // source 0.5 s + 5 frames
    }
    outer->multicamView = true;
    SequenceVideoSource grid(outer, false, 0, 0);
    const auto g = grid.frameAt(Rational(5, 25));
    ASSERT_TRUE(g.has_value() && g->composite);
    ASSERT_EQ(g->composite->layers.size(), 2u);
    EXPECT_NEAR(g->composite->layers[0].transform.scale, 50.0, 1e-6); // 2 columns
    EXPECT_GT(g->composite->layers[1].transform.positionX, g->composite->layers[0].transform.positionX);
}

TEST_F(Phase15SourceTest, PreviewKeysFollowEditsInTheirSegmentOnly) {
    auto s = snapshot();
    s->sequence.videoTracks[0].clips.push_back(clip(video, Rational(0), Rational(2)));
    s->sequence.videoTracks[0].clips.push_back(clip(video, Rational(6), Rational(2)));
    ASSERT_EQ(previewSegmentCount(*s), 4u);
    const auto k0 = previewSegmentKey(*s, 0);
    const auto k3 = previewSegmentKey(*s, 3);
    EXPECT_FALSE(k0.empty());
    EXPECT_TRUE(previewSegmentKey(*s, 1).empty() || previewSegmentKey(*s, 1) != k0);
    EXPECT_EQ(previewSegmentKey(*s, 0), k0); // stable
    auto edited = std::make_shared<SequenceSnapshot>(*s);
    edited->sequence.videoTracks[0].clips[1].sourceIn = Rational(1, 5);
    EXPECT_EQ(previewSegmentKey(*edited, 0), k0); // far from the edit
    EXPECT_NE(previewSegmentKey(*edited, 3), k3);
    const auto states = previewStates(*s, {});
    EXPECT_EQ(states[0], PreviewState::NotRendered);
}

} // namespace
