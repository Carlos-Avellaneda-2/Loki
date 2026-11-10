// Phase 13 model: clip speed math (D28), markers (D29), nested sequences (D30), format v3.
#include "model/project.h"
#include "model/project_json.h"

#include "model_fixtures.h"

#include <gtest/gtest.h>

#include <filesystem>

namespace {

using namespace loki;
using namespace loki::model;
using loki::test::makeClip;
using loki::test::makeMedia;

Clip retimed(Rational speed, bool reverse) {
    Clip c = makeClip(MediaId::generate(), Rational(10), Rational(4), Rational(2));
    c.speed = speed;
    c.reverse = reverse;
    return c;
}

TEST(ClipSpeedTest, ForwardSpeedScalesSourceTime) {
    const auto c = retimed(Rational(2), false); // 200 %: 4 s on the timeline show 8 s of source
    EXPECT_EQ(c.sourceSpan(), Rational(8));
    EXPECT_EQ(c.sourceOut(), Rational(10));
    EXPECT_EQ(c.toSourceTime(Rational(10)), Rational(2));
    EXPECT_EQ(c.toSourceTime(Rational(11)), Rational(4));
    EXPECT_EQ(c.toSequenceTime(Rational(4)), Rational(11));
}

TEST(ClipSpeedTest, ReverseReadsTheSpanBackwards) {
    const auto c = retimed(Rational(1, 2), true); // 50 % reversed: 2 s of source over 4 s
    EXPECT_EQ(c.sourceOut(), Rational(4));
    EXPECT_EQ(c.toSourceTime(Rational(10)), Rational(4));
    EXPECT_EQ(c.toSourceTime(Rational(14)), Rational(2));
    EXPECT_EQ(c.toSequenceTime(Rational(3)), Rational(12));
}

TEST(ClipSpeedTest, FrameHoldShowsSourceInEverywhere) {
    const auto c = retimed(Rational(), false);
    EXPECT_EQ(c.sourceSpan(), Rational());
    EXPECT_EQ(c.toSourceTime(Rational(13)), Rational(2));
    EXPECT_FALSE(c.roomBefore(Rational(60)).has_value()); // a hold extends freely
}

TEST(ClipSpeedTest, EdgeMovesKeepTheContentInPlace) {
    for (const bool reverse : {false, true}) {
        auto c = retimed(Rational(2), reverse);
        const Rational at = Rational(11);
        const Rational shown = c.toSourceTime(at + Rational(1, 2));
        auto left = c;
        left.moveOut(at - c.end());
        auto right = c;
        right.moveIn(at - c.start);
        EXPECT_EQ(left.end(), at);
        EXPECT_EQ(right.start, at);
        EXPECT_EQ(right.toSourceTime(at + Rational(1, 2)), shown) << reverse;
        EXPECT_EQ(left.toSourceTime(Rational(10)), c.toSourceTime(Rational(10))) << reverse;
        EXPECT_EQ(left.sourceSpan() + right.sourceSpan(), c.sourceSpan());
    }
}

TEST(ClipSpeedTest, RoomIsSourceLeftDividedBySpeed) {
    const auto c = retimed(Rational(2), false); // source [2, 10) of a 20 s media
    EXPECT_EQ(*c.roomBefore(Rational(20)), Rational(1));
    EXPECT_EQ(*c.roomAfter(Rational(20)), Rational(5));
    const auto r = retimed(Rational(2), true); // reversed: the head shows the source end
    EXPECT_EQ(*r.roomBefore(Rational(20)), Rational(5));
    EXPECT_EQ(*r.roomAfter(Rational(20)), Rational(1));
}

TEST(ModelPhase13Test, InvalidSpeedAndMarkersAreRejected) {
    ProjectBuilder builder;
    auto media = makeMedia("a");
    auto seq = makeDefaultSequence("S");
    auto c = makeClip(media.id, Rational(0), Rational(1));
    c.speed = Rational(-1);
    seq.videoTracks[0].clips.push_back(c);
    builder.addMedia(media);
    EXPECT_THROW(builder.addSequence(seq), ModelError);

    ProjectBuilder b2;
    auto m2 = makeMedia("b");
    Marker late{MarkerId::generate(), Rational(5), {}, "late", {}, MarkerColor::Red};
    Marker early{MarkerId::generate(), Rational(1), {}, "early", {}, MarkerColor::Blue};
    m2.markers = {late, early}; // not sorted
    EXPECT_THROW(b2.addMedia(m2), ModelError);
}

TEST(ModelPhase13Test, NestingCyclesAreForbidden) {
    ProjectBuilder builder;
    auto outer = makeDefaultSequence("Outer");
    auto inner = makeDefaultSequence("Inner");
    MediaItem nestInner;
    nestInner.id = MediaId::generate();
    nestInner.name = "Inner";
    nestInner.generator = std::string(kNestGenerator);
    nestInner.sequence = inner.id;
    nestInner.info.hasVideo = true;
    MediaItem nestOuter = nestInner;
    nestOuter.id = MediaId::generate();
    nestOuter.sequence = outer.id;
    builder.addMedia(nestInner).addMedia(nestOuter);
    outer.videoTracks[0].clips.push_back(makeClip(nestInner.id, Rational(0), Rational(1)));
    inner.videoTracks[0].clips.push_back(makeClip(nestOuter.id, Rational(0), Rational(1))); // cycle
    builder.addSequence(outer).addSequence(inner);
    EXPECT_THROW((void)builder.build(), ModelError);
}

TEST(ModelPhase13Test, SequenceContainsFollowsNestedClips) {
    ProjectBuilder builder;
    auto main = makeDefaultSequence("Main");
    auto inner = makeDefaultSequence("Inner");
    MediaItem nest;
    nest.id = MediaId::generate();
    nest.name = "Inner";
    nest.generator = std::string(kNestGenerator);
    nest.sequence = inner.id;
    nest.info.hasVideo = true;
    main.videoTracks[0].clips.push_back(makeClip(nest.id, Rational(0), Rational(2)));
    builder.addMedia(nest).addSequence(main).addSequence(inner);
    auto project = builder.build();
    EXPECT_TRUE(project.sequenceContains(inner.id, inner.id));
    EXPECT_TRUE(project.sequenceContains(main.id, inner.id));
    EXPECT_FALSE(project.sequenceContains(inner.id, main.id));
}

TEST(ModelPhase13Test, Version3RoundTripsSpeedMarkersAndNests) {
    ProjectBuilder builder;
    auto media = makeMedia("a");
    media.markers = {{MarkerId::generate(), Rational(3), Rational(1), "beat", "drop here", MarkerColor::Orange}};
    auto inner = makeDefaultSequence("Inner");
    MediaItem nest;
    nest.id = MediaId::generate();
    nest.name = "Inner";
    nest.generator = std::string(kNestGenerator);
    nest.sequence = inner.id;
    nest.info.hasVideo = true;
    auto seq = makeDefaultSequence("Main");
    auto c = makeClip(media.id, Rational(0), Rational(2), Rational(1));
    c.speed = Rational(3, 2);
    c.reverse = true;
    seq.videoTracks[0].clips.push_back(c);
    seq.videoTracks[1].clips.push_back(makeClip(nest.id, Rational(0), Rational(5)));
    seq.markers = {{MarkerId::generate(), Rational(1), {}, "", "", MarkerColor::Green},
                   {MarkerId::generate(), Rational(2), {}, "Chapter", "", MarkerColor::Cyan}};
    builder.addMedia(media).addMedia(nest).addSequence(seq).addSequence(inner);
    const auto project = builder.build();
    const auto text = serializeProject(project);
    EXPECT_NE(text.find("\"version\": " + std::to_string(kProjectFormatVersion)), std::string::npos);
    EXPECT_EQ(parseProject(text), project);
}

TEST(ModelPhase13Test, LoadsGoldenVersion3File) {
    const auto project = loadProjectFile(std::filesystem::path(LOKI_TEST_DATA_DIR) / "project_v3.loki");
    ASSERT_EQ(project.sequences().size(), 2u);
    const auto& main = project.sequences()[0];
    ASSERT_EQ(main.markers.size(), 1u);
    EXPECT_EQ(main.markers[0].name, "Intro");
    EXPECT_EQ(main.markers[0].color, MarkerColor::Red);
    const auto& retimedClip = main.videoTracks[0].clips[0];
    EXPECT_EQ(retimedClip.speed, Rational(1, 2));
    EXPECT_TRUE(retimedClip.reverse);
    const auto* nest = project.findMedia(main.videoTracks[1].clips[0].media);
    ASSERT_NE(nest, nullptr);
    EXPECT_TRUE(nest->isNest());
    EXPECT_EQ(nest->sequence, project.sequences()[1].id);
    const auto* adjustment = project.findMedia(main.videoTracks[2].clips[0].media);
    ASSERT_NE(adjustment, nullptr);
    EXPECT_TRUE(adjustment->isAdjustment());
    // Older documents still load (v1 and v2 migrate to v3).
    EXPECT_NO_THROW((void)loadProjectFile(std::filesystem::path(LOKI_TEST_DATA_DIR) / "project_v2.loki"));
    EXPECT_NO_THROW((void)loadProjectFile(std::filesystem::path(LOKI_TEST_DATA_DIR) / "project_v1.loki"));
}

} // namespace
