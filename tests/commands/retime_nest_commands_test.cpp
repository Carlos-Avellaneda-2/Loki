// Phase 13 commands: speed (D28), markers (D29), nest (D30), adjustment layers (D31).
#include "commands/edit_commands.h"
#include "commands/marker_commands.h"
#include "commands/project_commands.h"
#include "commands/undo_stack.h"
#include "model/project_json.h"

#include "model_fixtures.h"

#include <gtest/gtest.h>

namespace {

using namespace loki;
using namespace loki::model;
using namespace loki::commands;
using loki::test::dump;
using loki::test::makeClip;

// Sequence: V1, V2, A1 at 30 fps; media 60 s.
class RetimeNestTest : public ::testing::Test {
protected:
    test::BasicProject ids;
    Project project = ids.build();
    UndoStack stack{project};

    ClipId add(TrackId track, Rational start, Rational duration, Rational sourceIn = {}, LinkId link = {}) {
        auto clip = makeClip(ids.media, start, duration, sourceIn);
        clip.link = link;
        auto cmd = std::make_unique<AddClipCommand>(track, clip);
        const auto id = cmd->clipId();
        stack.push(std::move(cmd));
        return id;
    }
    const Clip& clip(ClipId id) { return *project.findClip(id).clip; }
    const std::vector<Clip>& clipsOf(TrackId track) { return project.findTrack(track).track->clips; }
    void pushVerified(std::unique_ptr<Command> command) {
        ASSERT_NE(command, nullptr);
        const auto before = dump(project);
        stack.push(std::move(command));
        ASSERT_TRUE(project.validate().empty()) << project.validate().front();
        const auto after = dump(project);
        stack.undo();
        ASSERT_EQ(dump(project), before);
        stack.redo();
        ASSERT_EQ(dump(project), after);
    }
};

TEST_F(RetimeNestTest, SpeedChangesTheDurationAndKeepsTheContent) {
    const auto a = add(ids.v1, Rational(0), Rational(4), Rational(10));
    pushVerified(makeSetClipSpeed(project, ids.sequence, {a}, Rational(2), false, false));
    EXPECT_EQ(clip(a).duration, Rational(2));
    EXPECT_EQ(clip(a).speed, Rational(2));
    EXPECT_EQ(clip(a).sourceIn, Rational(10));
    EXPECT_EQ(clip(a).sourceOut(), Rational(14));
    pushVerified(makeSetClipSpeed(project, ids.sequence, {a}, Rational(1, 2), true, false));
    EXPECT_EQ(clip(a).duration, Rational(8));
    EXPECT_TRUE(clip(a).reverse);
    EXPECT_EQ(clip(a).toSourceTime(Rational(0)), Rational(14)); // reversed: starts at the end
}

TEST_F(RetimeNestTest, SlowerClipStopsAtTheNextClipUnlessRippling) {
    const auto a = add(ids.v1, Rational(0), Rational(4), Rational(10));
    const auto b = add(ids.v1, Rational(5), Rational(2));
    const auto c = add(ids.a1, Rational(6), Rational(1));
    pushVerified(makeSetClipSpeed(project, ids.sequence, {a}, Rational(1, 2), false, false));
    EXPECT_EQ(clip(a).duration, Rational(5)); // would be 8, stops at b
    EXPECT_EQ(clip(b).start, Rational(5));
    stack.undo();
    pushVerified(makeSetClipSpeed(project, ids.sequence, {a}, Rational(1, 2), false, true));
    EXPECT_EQ(clip(a).duration, Rational(8));
    EXPECT_EQ(clip(b).start, Rational(9)); // same track follows
    EXPECT_EQ(clip(c).start, Rational(6)); // other tracks stay
}

TEST_F(RetimeNestTest, FasterClipIsLimitedByTheMedia) {
    const auto a = add(ids.v1, Rational(0), Rational(10), Rational(50)); // source [50, 60) of 60 s
    pushVerified(makeSetClipSpeed(project, ids.sequence, {a}, Rational(1, 2), false, true));
    EXPECT_EQ(clip(a).duration, Rational(20)); // slower: no limit issue
    stack.undo();
    // A frame hold stretched to 20 s, set back to 100 %, cannot read past the media end.
    const auto b = add(ids.v2, Rational(0), Rational(5), Rational(50));
    pushVerified(makeAddFrameHold(project, ids.sequence, b, Rational(0)));
    pushVerified(makeTrimEdge(project, ids.sequence, b, ClipEdge::Out, Rational(15)));
    EXPECT_EQ(clip(b).duration, Rational(20));
    pushVerified(makeSetClipSpeed(project, ids.sequence, {b}, Rational(1), false, false));
    EXPECT_EQ(clip(b).duration, Rational(10));
    EXPECT_EQ(clip(b).sourceOut(), Rational(60));
}

TEST_F(RetimeNestTest, TrimsAndSplitsRespectSpeed) {
    const auto a = add(ids.v1, Rational(0), Rational(4), Rational(10));
    pushVerified(makeSetClipSpeed(project, ids.sequence, {a}, Rational(2), false, false)); // 2 s, source [10, 14)
    // Trim In can reach back 10 s of source = 5 s of timeline at 200 %, but not before 0.
    const auto moved = add(ids.v2, Rational(10), Rational(2), Rational(10));
    pushVerified(makeSetClipSpeed(project, ids.sequence, {moved}, Rational(2), false, false));
    pushVerified(makeTrimEdge(project, ids.sequence, moved, ClipEdge::In, Rational(-30)));
    EXPECT_EQ(clip(moved).start, Rational(5));
    EXPECT_EQ(clip(moved).sourceIn, Rational(0));
    // Razor in the middle keeps the content continuous.
    pushVerified(makeRazorEdit(project, ids.sequence, {a}, Rational(1)));
    ASSERT_EQ(clipsOf(ids.v1).size(), 2u);
    EXPECT_EQ(clipsOf(ids.v1)[1].sourceIn, Rational(12));
    EXPECT_EQ(clipsOf(ids.v1)[1].speed, Rational(2));
}

TEST_F(RetimeNestTest, RateStretchChangesSpeedNotContent) {
    const auto a = add(ids.v1, Rational(0), Rational(4), Rational(10));
    pushVerified(makeRateStretch(project, ids.sequence, a, ClipEdge::Out, Rational(4)));
    EXPECT_EQ(clip(a).duration, Rational(8));
    EXPECT_EQ(clip(a).speed, Rational(1, 2));
    EXPECT_EQ(clip(a).sourceOut(), Rational(14));
    pushVerified(makeRateStretch(project, ids.sequence, a, ClipEdge::In, Rational(6)));
    EXPECT_EQ(clip(a).start, Rational(6));
    EXPECT_EQ(clip(a).speed, Rational(2));
    EXPECT_EQ(clip(a).sourceIn, Rational(10));
}

TEST_F(RetimeNestTest, FrameHoldFreezesFromThePlayhead) {
    const auto link = LinkId::generate();
    const auto a = add(ids.v1, Rational(0), Rational(4), Rational(10), link);
    const auto audio = add(ids.a1, Rational(0), Rational(4), Rational(10), link);
    pushVerified(makeAddFrameHold(project, ids.sequence, a, Rational(1)));
    ASSERT_EQ(clipsOf(ids.v1).size(), 2u);
    const auto& hold = clipsOf(ids.v1)[1];
    EXPECT_EQ(hold.start, Rational(1));
    EXPECT_EQ(hold.duration, Rational(3));
    EXPECT_TRUE(hold.speed.isZero());
    EXPECT_EQ(hold.sourceIn, Rational(11));
    EXPECT_EQ(hold.toSourceTime(Rational(3)), Rational(11));
    EXPECT_EQ(clip(audio).duration, Rational(4)); // audio continues
    EXPECT_THROW((void)makeAddFrameHold(project, ids.sequence, audio, Rational(1)), CommandError);
}

TEST_F(RetimeNestTest, SequenceMarkersAddEditRemoveAndNavigate) {
    pushVerified(makeAddSequenceMarker(project, ids.sequence, {MarkerId(), Rational(5), {}, "b", {}, MarkerColor::Green}));
    pushVerified(makeAddSequenceMarker(project, ids.sequence, {MarkerId(), Rational(2), {}, "a", {}, MarkerColor::Red}));
    const auto& markers = project.findSequence(ids.sequence)->markers;
    ASSERT_EQ(markers.size(), 2u);
    EXPECT_EQ(markers[0].name, "a"); // sorted by time
    EXPECT_EQ(*nextMarkerTime(markers, Rational(2)), Rational(5));
    EXPECT_EQ(*previousMarkerTime(markers, Rational(5)), Rational(2));
    EXPECT_FALSE(nextMarkerTime(markers, Rational(5)).has_value());
    auto edited = markers[1];
    edited.comment = "chorus";
    edited.color = MarkerColor::Cyan;
    pushVerified(makeEditSequenceMarker(project, ids.sequence, edited));
    EXPECT_EQ(project.findSequence(ids.sequence)->markers[1].comment, "chorus");
    pushVerified(makeRemoveSequenceMarker(project, ids.sequence, markers[0].id));
    EXPECT_EQ(project.findSequence(ids.sequence)->markers.size(), 1u);
    pushVerified(makeClearSequenceMarkers(project, ids.sequence));
    EXPECT_TRUE(project.findSequence(ids.sequence)->markers.empty());
}

TEST_F(RetimeNestTest, ClipMarkersLiveOnTheMedia) {
    pushVerified(makeAddMediaMarker(project, ids.media, {MarkerId(), Rational(7), {}, "laugh", {}, MarkerColor::Yellow}));
    const auto& markers = project.findMedia(ids.media)->markers;
    ASSERT_EQ(markers.size(), 1u);
    pushVerified(makeRemoveMediaMarker(project, ids.media, markers[0].id));
    EXPECT_TRUE(project.findMedia(ids.media)->markers.empty());
}

TEST_F(RetimeNestTest, NestMovesClipsIntoANewSequenceAndUndoes) {
    const auto link = LinkId::generate();
    const auto a = add(ids.v1, Rational(2), Rational(3), Rational(10), link);
    const auto audio = add(ids.a1, Rational(2), Rational(3), Rational(10), link);
    const auto b = add(ids.v2, Rational(4), Rational(2));
    const auto other = add(ids.v1, Rational(8), Rational(1));
    MediaId nestMedia;
    SequenceId nested;
    pushVerified(makeNest(project, ids.sequence, {a, b}, "Nested", &nestMedia, &nested));
    const auto* inner = project.findSequence(nested);
    ASSERT_NE(inner, nullptr);
    ASSERT_EQ(inner->videoTracks.size(), 2u);
    ASSERT_EQ(inner->videoTracks[0].clips.size(), 1u);
    EXPECT_EQ(inner->videoTracks[0].clips[0].start, Rational(0));
    EXPECT_EQ(inner->videoTracks[1].clips[0].start, Rational(2));
    EXPECT_EQ(inner->audioTracks[0].clips.size(), 1u); // linked audio came along
    EXPECT_EQ(project.findClip(a).clip, nullptr);
    EXPECT_EQ(project.findClip(audio).clip, nullptr);
    // The parent holds one linked nested clip per kind on the lowest track used.
    ASSERT_EQ(clipsOf(ids.v1).size(), 2u);
    const auto& nestClip = clipsOf(ids.v1)[0];
    EXPECT_EQ(nestClip.media, nestMedia);
    EXPECT_EQ(nestClip.start, Rational(2));
    EXPECT_EQ(nestClip.duration, Rational(4));
    ASSERT_EQ(clipsOf(ids.a1).size(), 1u);
    EXPECT_EQ(clipsOf(ids.a1)[0].link, nestClip.link);
    EXPECT_TRUE(clipsOf(ids.v2).empty());
    EXPECT_EQ(clip(other).start, Rational(8));
    EXPECT_TRUE(project.sequenceContains(ids.sequence, nested));
    // Nesting the parent into its own child is refused.
    const auto inside = project.findSequence(nested)->videoTracks[0].clips[0].id;
    EXPECT_THROW(stack.push(std::make_unique<AddClipCommand>(project.findSequence(nested)->videoTracks[1].id,
                                                             makeClip(nestMedia, Rational(10), Rational(1)))),
                 std::exception);
    (void)inside;
}

TEST_F(RetimeNestTest, AdjustmentLayerIsAGeneratedVideoItem) {
    MediaId media;
    pushVerified(makeNewGeneratedClip(project, ids.sequence, ids.v2, Rational(1), Rational(5), "Adjustment Layer",
                                      kAdjustmentGenerator, &media));
    const auto* item = project.findMedia(media);
    ASSERT_NE(item, nullptr);
    EXPECT_TRUE(item->isAdjustment());
    ASSERT_EQ(clipsOf(ids.v2).size(), 1u);
    EXPECT_EQ(clipsOf(ids.v2)[0].duration, Rational(5));
    // Trims are not limited by a media duration.
    pushVerified(makeTrimEdge(project, ids.sequence, clipsOf(ids.v2)[0].id, ClipEdge::Out, Rational(20)));
    EXPECT_EQ(clipsOf(ids.v2)[0].duration, Rational(25));
}

TEST_F(RetimeNestTest, RemovingANestedSequenceNeedsItsItemGoneFirst) {
    const auto a = add(ids.v1, Rational(0), Rational(2));
    MediaId nestMedia;
    SequenceId nested;
    pushVerified(makeNest(project, ids.sequence, {a}, "N", &nestMedia, &nested));
    EXPECT_THROW(stack.push(std::make_unique<RemoveSequenceCommand>(nested)), std::exception);
    std::vector<std::unique_ptr<Command>> steps;
    steps.push_back(std::make_unique<RemoveMediaCommand>(nestMedia));
    steps.push_back(std::make_unique<RemoveSequenceCommand>(nested));
    pushVerified(std::make_unique<MacroCommand>("Delete Sequence", std::move(steps)));
    EXPECT_EQ(project.findSequence(nested), nullptr);
    EXPECT_TRUE(clipsOf(ids.v1).empty());
}

} // namespace
