// Phase 13 playback: retimed clips (D28), nested sequences (D30), adjustment layers (D31).
#include "audio/mixer.h"
#include "media/media_probe.h"
#include "model/effect_registry.h"
#include "model/project.h"
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

model::Clip clip(const model::MediaItem& media, Rational start, Rational duration, Rational sourceIn = {},
                 Rational speed = Rational(1), bool reverse = false) {
    model::Clip c;
    c.id = model::ClipId::generate();
    c.media = media.id;
    c.start = start;
    c.duration = duration;
    c.sourceIn = sourceIn;
    c.speed = speed;
    c.reverse = reverse;
    return c;
}

int luma(const media::VideoFramePtr& f) {
    return f ? f->planes[0][f->planes[0].size() / 2] : -1;
}

model::MediaItem generated(std::string_view generator) {
    model::MediaItem m;
    m.id = model::MediaId::generate();
    m.name = std::string(generator);
    m.generator = std::string(generator);
    m.info.hasVideo = true;
    m.info.hasAudio = generator == model::kNestGenerator;
    m.info.isStill = generator == model::kAdjustmentGenerator;
    m.info.width = 1920;
    m.info.height = 1080;
    return m;
}

// Zero crossings per second of the left channel (2 x the frequency of a sine).
double crossingsPerSecond(const std::vector<float>& buf, std::size_t frames, int rate) {
    int crossings = 0;
    for (std::size_t i = 1; i < frames; ++i) {
        if ((buf[(i - 1) * 2] < 0.0f) != (buf[i * 2] < 0.0f)) {
            ++crossings;
        }
    }
    return crossings * static_cast<double>(rate) / static_cast<double>(frames);
}

class RetimeNestSourceTest : public ::testing::Test {
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

    std::vector<float> readAudio(const std::shared_ptr<const SequenceSnapshot>& s, Rational from, std::size_t frames) {
        SequenceAudioSource src(s, 48000, 2);
        src.seek(from);
        std::vector<float> buf(frames * 2, 0.0f);
        std::size_t done = 0;
        while (done < frames) { // small blocks, like the device callback
            const auto n = src.read(buf.data() + done * 2, std::min<std::size_t>(512, frames - done));
            if (n == 0) {
                break;
            }
            done += n;
        }
        return buf;
    }
};

TEST_F(RetimeNestSourceTest, DoubleSpeedSkipsFrames) {
    auto s = snapshot();
    s->sequence.videoTracks[0].clips.push_back(clip(video, Rational(0), Rational(1), Rational(), Rational(2)));
    SequenceVideoSource src(s, false, 0, 0);
    EXPECT_NEAR(luma(src.frameAt(Rational(5, 25))->frame), expectedLuma(10), 3);
    EXPECT_NEAR(luma(src.frameAt(Rational(12, 25))->frame), expectedLuma(24), 3);
}

TEST_F(RetimeNestSourceTest, ReverseShowsFramesBackwards) {
    auto s = snapshot();
    s->sequence.videoTracks[0].clips.push_back(clip(video, Rational(0), Rational(1), Rational(), Rational(1), true));
    SequenceVideoSource src(s, false, 0, 0);
    EXPECT_NEAR(luma(src.frameAt(Rational(0))->frame), expectedLuma(24), 3);
    EXPECT_NEAR(luma(src.frameAt(Rational(10, 25))->frame), expectedLuma(14), 3);
    // Sequential playback walks the source backwards.
    src.seek(Rational(20, 25));
    for (int i = 20; i < 25; ++i) {
        const auto f = src.nextFrame();
        ASSERT_TRUE(f.has_value());
        EXPECT_NEAR(luma(f->frame), expectedLuma(24 - i), 3) << "frame " << i;
    }
}

TEST_F(RetimeNestSourceTest, FrameHoldRepeatsOneFrame) {
    auto s = snapshot();
    s->sequence.videoTracks[0].clips.push_back(clip(video, Rational(0), Rational(1), Rational(10, 25), Rational()));
    SequenceVideoSource src(s, false, 0, 0);
    EXPECT_NEAR(luma(src.frameAt(Rational(0))->frame), expectedLuma(10), 3);
    EXPECT_NEAR(luma(src.frameAt(Rational(20, 25))->frame), expectedLuma(10), 3);
}

TEST_F(RetimeNestSourceTest, NestedSequenceIsATransparentSubComposite) {
    auto inner = snapshot();
    inner->sequence.videoTracks[0].clips.push_back(clip(video, Rational(0), Rational(1), Rational(10, 25)));
    auto nest = generated(model::kNestGenerator);
    nest.sequence = inner->sequence.id;
    auto outer = snapshot();
    outer->media[nest.id] = nest;
    outer->nested[inner->sequence.id] = inner;
    outer->sequence.videoTracks[1].clips.push_back(clip(nest, Rational(1), Rational(1)));
    SequenceVideoSource src(outer, false, 0, 0);
    const auto f = src.frameAt(Rational(1) + Rational(5, 25));
    ASSERT_TRUE(f.has_value());
    ASSERT_TRUE(f->composite);
    ASSERT_EQ(f->composite->layers.size(), 1u);
    const auto& layer = f->composite->layers[0];
    ASSERT_TRUE(layer.nested);
    EXPECT_TRUE(layer.nested->transparent);
    ASSERT_EQ(layer.nested->layers.size(), 1u);
    EXPECT_NEAR(luma(layer.nested->layers[0].frame), expectedLuma(15), 3);
    EXPECT_EQ(layer.sourceWidth, 1920.0);
    // Past the nested sequence's end the nest shows nothing.
    outer->sequence.videoTracks[1].clips[0].duration = Rational(3);
    SequenceVideoSource longer(outer, false, 0, 0);
    const auto past = longer.frameAt(Rational(5, 2));
    ASSERT_TRUE(past.has_value());
    EXPECT_FALSE(past->composite);
}

TEST_F(RetimeNestSourceTest, AdjustmentLayerCarriesItsGradeOverTheCanvas) {
    auto s = snapshot();
    auto adjustment = generated(model::kAdjustmentGenerator);
    s->media[adjustment.id] = adjustment;
    s->sequence.videoTracks[0].clips.push_back(clip(video, Rational(0), Rational(1)));
    auto adj = clip(adjustment, Rational(0), Rational(1));
    auto color = model::makeEffect(model::kColorEffect, adjustment.info, s->sequence.settings);
    for (auto& p : color.parameters) {
        if (p.key == "saturation") {
            p.value = 0.0;
        }
    }
    adj.effects.push_back(color);
    s->sequence.videoTracks[1].clips.push_back(adj);
    SequenceVideoSource src(s, false, 0, 0);
    const auto f = src.frameAt(Rational(5, 25));
    ASSERT_TRUE(f.has_value() && f->composite);
    ASSERT_EQ(f->composite->layers.size(), 2u);
    const auto& layer = f->composite->layers[1];
    EXPECT_TRUE(layer.adjustment);
    EXPECT_FALSE(layer.frame);
    EXPECT_EQ(layer.grade.color.saturation, 0.0f);
    EXPECT_EQ(layer.sourceWidth, static_cast<double>(s->sequence.settings.width));
    EXPECT_TRUE(f->frame); // the decoded picture below is still the timed frame
}

TEST_F(RetimeNestSourceTest, AudioFollowsSpeedAndHoldsAreSilent) {
    auto s = snapshot();
    s->sequence.audioTracks[0].clips.push_back(clip(tone, Rational(0), Rational(1, 2), Rational(), Rational(2)));
    const auto fast = readAudio(s, Rational(0), 9600); // 0.2 s
    EXPECT_NEAR(crossingsPerSecond(fast, 9600, 48000), 2.0 * 880.0, 40.0); // 440 Hz -> 880 Hz
    EXPECT_NEAR(audio::measure(fast.data(), 9600, 2).left.peak, 0.5f, 0.03f);

    auto slow = snapshot();
    slow->sequence.audioTracks[0].clips.push_back(clip(tone, Rational(0), Rational(2), Rational(), Rational(1, 2), true));
    const auto reversed = readAudio(slow, Rational(1, 2), 9600);
    EXPECT_NEAR(crossingsPerSecond(reversed, 9600, 48000), 2.0 * 220.0, 20.0);
    EXPECT_NEAR(audio::measure(reversed.data(), 9600, 2).left.peak, 0.5f, 0.03f);

    auto hold = snapshot();
    hold->sequence.audioTracks[0].clips.push_back(clip(tone, Rational(0), Rational(1), Rational(), Rational()));
    const auto silent = readAudio(hold, Rational(0), 4800);
    EXPECT_EQ(audio::measure(silent.data(), 4800, 2).left.peak, 0.0f);
}

TEST_F(RetimeNestSourceTest, NestedSequenceAudioIsMixedIn) {
    auto inner = snapshot();
    inner->sequence.audioTracks[0].clips.push_back(clip(tone, Rational(0), Rational(1)));
    auto nest = generated(model::kNestGenerator);
    nest.sequence = inner->sequence.id;
    auto outer = snapshot();
    outer->media[nest.id] = nest;
    outer->nested[inner->sequence.id] = inner;
    outer->sequence.audioTracks[0].clips.push_back(clip(nest, Rational(1), Rational(1)));
    const auto before = readAudio(outer, Rational(1, 2), 4800);
    EXPECT_EQ(audio::measure(before.data(), 4800, 2).left.peak, 0.0f);
    const auto inside = readAudio(outer, Rational(5, 4), 4800);
    EXPECT_NEAR(audio::measure(inside.data(), 4800, 2).left.peak, 0.5f, 0.03f);
    EXPECT_NEAR(crossingsPerSecond(inside, 4800, 48000), 880.0, 30.0);
}

TEST_F(RetimeNestSourceTest, SnapshotsIncludeNestedSequencesAndCompareThem) {
    model::ProjectBuilder builder;
    auto inner = model::makeDefaultSequence("Inner");
    inner.videoTracks[0].clips.push_back(clip(video, Rational(0), Rational(1)));
    auto nest = generated(model::kNestGenerator);
    nest.sequence = inner.id;
    auto main = model::makeDefaultSequence("Main");
    main.videoTracks[0].clips.push_back(clip(nest, Rational(0), Rational(1)));
    builder.addMedia(video).addMedia(nest).addSequence(main).addSequence(inner);
    const auto project = builder.build();
    const auto a = makeSnapshot(project, main.id, 1);
    ASSERT_TRUE(a);
    ASSERT_EQ(a->nested.size(), 1u);
    ASSERT_TRUE(a->nested.at(inner.id));
    EXPECT_TRUE(a->nested.at(inner.id)->media.contains(video.id));
    const auto b = makeSnapshot(project, main.id, 2);
    EXPECT_TRUE(sameExceptAudioMix(*a, *b));
    auto changed = std::make_shared<SequenceSnapshot>(*b);
    auto innerCopy = std::make_shared<SequenceSnapshot>(*changed->nested.at(inner.id));
    innerCopy->sequence.videoTracks[0].clips[0].start = Rational(1, 2);
    changed->nested[inner.id] = innerCopy;
    EXPECT_FALSE(sameExceptAudioMix(*a, *changed));
}

} // namespace
