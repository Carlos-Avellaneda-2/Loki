#include "commands/edit_commands.h"
#include "commands/project_commands.h"
#include "commands/undo_stack.h"

#include "model_fixtures.h"

#include <gtest/gtest.h>

namespace {

using namespace loki;
using namespace loki::model;
using namespace loki::commands;
using loki::test::dump;
using loki::test::makeClip;

class EditCommandsTest : public ::testing::Test {
protected:
    test::BasicProject ids; // media: 60 s A/V; sequence: V1, V2, A1 at 30 fps
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

    const std::vector<Clip>& clipsOf(TrackId track) { return project.findTrack(track).track->clips; }

    void pushVerified(std::unique_ptr<Command> command) {
        ASSERT_NE(command, nullptr);
        const auto before = dump(project);
        stack.push(std::move(command));
        const auto after = dump(project);
        ASSERT_TRUE(project.validate().empty()) << project.validate().front();
        stack.undo();
        ASSERT_EQ(dump(project), before);
        stack.redo();
        ASSERT_EQ(dump(project), after);
    }

    SourceRange range(Rational in, Rational duration) { return SourceRange{ids.media, in, duration}; }
};

TEST_F(EditCommandsTest, SourceRangeUsesMarks) {
    auto item = *project.findMedia(ids.media);
    auto r = sourceRangeOf(item);
    EXPECT_EQ(r.sourceIn, Rational());
    EXPECT_EQ(r.duration, Rational(60));
    item.markIn = Rational(10);
    item.markOut = Rational(15);
    r = sourceRangeOf(item);
    EXPECT_EQ(r.sourceIn, Rational(10));
    EXPECT_EQ(r.duration, Rational(5));
    item.info.isStill = true;
    item.markIn.reset();
    item.markOut.reset();
    EXPECT_EQ(sourceRangeOf(item).duration, Rational(5)); // default still duration
}

TEST_F(EditCommandsTest, OverwriteIntoEmptyTracksPlacesLinkedPair) {
    pushVerified(makeOverwriteEdit(project, ids.sequence, range(Rational(2), Rational(3)), Rational(10), {ids.v1, ids.a1}));
    ASSERT_EQ(clipsOf(ids.v1).size(), 1u);
    ASSERT_EQ(clipsOf(ids.a1).size(), 1u);
    const auto& v = clipsOf(ids.v1)[0];
    const auto& a = clipsOf(ids.a1)[0];
    EXPECT_EQ(v.start, Rational(10));
    EXPECT_EQ(v.sourceIn, Rational(2));
    EXPECT_EQ(v.duration, Rational(3));
    EXPECT_FALSE(v.link.isNull());
    EXPECT_EQ(v.link, a.link);
    EXPECT_EQ(stack.undoText(), "Overwrite");
}

TEST_F(EditCommandsTest, OverwriteInsideAClipSplitsIt) {
    add(ids.v1, Rational(0), Rational(10));
    pushVerified(makeOverwriteEdit(project, ids.sequence, range(Rational(30), Rational(2)), Rational(4), {ids.v1, {}}));
    const auto& clips = clipsOf(ids.v1);
    ASSERT_EQ(clips.size(), 3u);
    EXPECT_EQ(clips[0].start, Rational(0));
    EXPECT_EQ(clips[0].duration, Rational(4));
    EXPECT_EQ(clips[1].start, Rational(4));
    EXPECT_EQ(clips[1].sourceIn, Rational(30));
    EXPECT_EQ(clips[2].start, Rational(6));
    EXPECT_EQ(clips[2].sourceIn, Rational(6)); // continues where it would have
    EXPECT_EQ(clips[2].end(), Rational(10));
}

TEST_F(EditCommandsTest, OverwriteTrimsAndRemovesCoveredClips) {
    add(ids.v1, Rational(0), Rational(4));
    add(ids.v1, Rational(5), Rational(2));
    add(ids.v1, Rational(8), Rational(4));
    pushVerified(makeOverwriteEdit(project, ids.sequence, range(Rational(0), Rational(6)), Rational(3), {ids.v1, {}}));
    const auto& clips = clipsOf(ids.v1);
    ASSERT_EQ(clips.size(), 3u);
    EXPECT_EQ(clips[0].end(), Rational(3));   // trimmed tail
    EXPECT_EQ(clips[1].start, Rational(3));   // the new clip
    EXPECT_EQ(clips[2].start, Rational(9));   // head trimmed
    EXPECT_EQ(clips[2].sourceIn, Rational(1));
    EXPECT_EQ(clips[2].end(), Rational(12));
}

TEST_F(EditCommandsTest, InsertRipplesUnlockedTracksAndSplits) {
    add(ids.v1, Rational(0), Rational(10));
    add(ids.v2, Rational(6), Rational(2));
    add(ids.a1, Rational(20), Rational(1));
    pushVerified(makeInsertEdit(project, ids.sequence, range(Rational(0), Rational(3)), Rational(4), {ids.v1, {}}));
    const auto& v1 = clipsOf(ids.v1);
    ASSERT_EQ(v1.size(), 3u);
    EXPECT_EQ(v1[0].end(), Rational(4));
    EXPECT_EQ(v1[1].start, Rational(4)); // inserted
    EXPECT_EQ(v1[2].start, Rational(7)); // right part pushed by 3
    EXPECT_EQ(v1[2].sourceIn, Rational(4));
    EXPECT_EQ(clipsOf(ids.v2)[0].start, Rational(9));
    EXPECT_EQ(clipsOf(ids.a1)[0].start, Rational(23));
}

TEST_F(EditCommandsTest, InsertLeavesLockedTracksAloneAndRejectsLockedTargets) {
    add(ids.v2, Rational(6), Rational(2));
    stack.push(setTrackLocked(project, ids.v2, true));
    pushVerified(makeInsertEdit(project, ids.sequence, range(Rational(0), Rational(3)), Rational(4), {ids.v1, {}}));
    EXPECT_EQ(clipsOf(ids.v2)[0].start, Rational(6));
    EXPECT_THROW((void)makeInsertEdit(project, ids.sequence, range(Rational(0), Rational(1)), Rational(0), {ids.v2, {}}),
                 CommandError);
}

TEST_F(EditCommandsTest, EditValidation) {
    EXPECT_THROW((void)makeOverwriteEdit(project, ids.sequence, range(Rational(0), Rational(0)), Rational(0), {ids.v1, {}}),
                 CommandError);
    EXPECT_THROW((void)makeOverwriteEdit(project, ids.sequence, range(Rational(0), Rational(1)), Rational(-1), {ids.v1, {}}),
                 CommandError);
    EXPECT_THROW((void)makeOverwriteEdit(project, ids.sequence, range(Rational(0), Rational(1)), Rational(0), {ids.a1, {}}),
                 CommandError); // audio track as video target
    EXPECT_THROW((void)makeOverwriteEdit(project, ids.sequence, range(Rational(0), Rational(1)), Rational(0), {}),
                 CommandError);
    EXPECT_THROW((void)makeOverwriteEdit(project, SequenceId::generate(), range(Rational(0), Rational(1)), Rational(0),
                                         {ids.v1, {}}),
                 CommandError);
}

TEST_F(EditCommandsTest, RazorSplitsLinkedPairAndKeepsBothHalvesLinked) {
    const auto link = LinkId::generate();
    const auto v = add(ids.v1, Rational(0), Rational(10), Rational(), link);
    add(ids.a1, Rational(0), Rational(10), Rational(), link);
    pushVerified(makeRazorEdit(project, ids.sequence, {v}, Rational(4)));
    ASSERT_EQ(clipsOf(ids.v1).size(), 2u);
    ASSERT_EQ(clipsOf(ids.a1).size(), 2u);
    EXPECT_EQ(clipsOf(ids.v1)[0].link, link);
    EXPECT_EQ(clipsOf(ids.a1)[0].link, link);
    EXPECT_FALSE(clipsOf(ids.v1)[1].link.isNull());
    EXPECT_NE(clipsOf(ids.v1)[1].link, link);
    EXPECT_EQ(clipsOf(ids.v1)[1].link, clipsOf(ids.a1)[1].link);
    EXPECT_EQ(makeRazorEdit(project, ids.sequence, {v}, Rational(20)), nullptr); // outside: no-op
}

TEST_F(EditCommandsTest, RazorAllTracks) {
    add(ids.v1, Rational(0), Rational(10));
    add(ids.v2, Rational(2), Rational(10));
    add(ids.a1, Rational(6), Rational(1));
    pushVerified(makeRazorAllTracks(project, ids.sequence, Rational(5)));
    EXPECT_EQ(clipsOf(ids.v1).size(), 2u);
    EXPECT_EQ(clipsOf(ids.v2).size(), 2u);
    EXPECT_EQ(clipsOf(ids.a1).size(), 1u);
}

TEST_F(EditCommandsTest, MoveLinkedClipsAndOverwriteDestination) {
    const auto link = LinkId::generate();
    const auto v = add(ids.v1, Rational(0), Rational(4), Rational(), link);
    add(ids.a1, Rational(0), Rational(4), Rational(), link);
    add(ids.v2, Rational(10), Rational(4)); // will be partly overwritten
    pushVerified(makeMoveClips(project, ids.sequence, {v}, Rational(8), 1, TrackKind::Video));
    ASSERT_EQ(clipsOf(ids.v1).size(), 0u);
    const auto& v2 = clipsOf(ids.v2);
    ASSERT_EQ(v2.size(), 2u);
    EXPECT_EQ(v2[0].start, Rational(8));
    EXPECT_EQ(v2[0].id, v);
    EXPECT_EQ(v2[1].start, Rational(12)); // overwritten head
    EXPECT_EQ(clipsOf(ids.a1)[0].start, Rational(8)); // linked audio moved in time, same track
}

TEST_F(EditCommandsTest, MoveValidation) {
    const auto v = add(ids.v1, Rational(1), Rational(2));
    EXPECT_THROW((void)makeMoveClips(project, ids.sequence, {v}, Rational(-2), 0, TrackKind::Video), CommandError);
    EXPECT_THROW((void)makeMoveClips(project, ids.sequence, {v}, Rational(0), 5, TrackKind::Video), CommandError);
    stack.push(setTrackLocked(project, ids.v2, true));
    EXPECT_THROW((void)makeMoveClips(project, ids.sequence, {v}, Rational(0), 1, TrackKind::Video), CommandError);
}

TEST_F(EditCommandsTest, DragMovesMergeIntoOneUndoStep) {
    const auto v = add(ids.v1, Rational(0), Rational(2));
    const auto before = dump(project);
    for (int i = 1; i <= 5; ++i) {
        // Each step is computed from the current project, like the timeline does while dragging.
        stack.push(makeMoveClips(project, ids.sequence, {v}, Rational(1), 0, TrackKind::Video, GestureId{42}));
    }
    EXPECT_EQ(clipsOf(ids.v1)[0].start, Rational(5));
    stack.undo();
    EXPECT_EQ(dump(project), before);
    stack.redo();
    EXPECT_EQ(clipsOf(ids.v1)[0].start, Rational(5));
}

TEST_F(EditCommandsTest, TrimClampsToMediaNeighboursAndMinimum) {
    const auto a = add(ids.v1, Rational(0), Rational(5), Rational(2));
    add(ids.v1, Rational(8), Rational(2));
    // Out edge: neighbour at 8 limits the extension to +3.
    pushVerified(makeTrimEdge(project, ids.sequence, a, ClipEdge::Out, Rational(10)));
    EXPECT_EQ(clipsOf(ids.v1)[0].end(), Rational(8));
    // In edge: source starts at 2 and sequence at 0: can only go back 0 (start is 0).
    EXPECT_EQ(makeTrimEdge(project, ids.sequence, a, ClipEdge::In, Rational(-5)), nullptr);
    // In edge forward: min duration is one frame (1/30).
    pushVerified(makeTrimEdge(project, ids.sequence, a, ClipEdge::In, Rational(100)));
    EXPECT_EQ(clipsOf(ids.v1)[0].duration, Rational(1, 30));
    EXPECT_EQ(clipsOf(ids.v1)[0].sourceOut(), Rational(10));
}

TEST_F(EditCommandsTest, TrimRespectsMediaDurationAndLinkedPartner) {
    const auto link = LinkId::generate();
    const auto v = add(ids.v1, Rational(0), Rational(50), Rational(5), link);
    add(ids.a1, Rational(0), Rational(50), Rational(5), link);
    pushVerified(makeTrimEdge(project, ids.sequence, v, ClipEdge::Out, Rational(30)));
    EXPECT_EQ(clipsOf(ids.v1)[0].sourceOut(), Rational(60)); // media is 60 s long
    EXPECT_EQ(clipsOf(ids.a1)[0].sourceOut(), Rational(60));
    pushVerified(makeTrimEdge(project, ids.sequence, v, ClipEdge::Out, Rational(-10), false));
    EXPECT_EQ(clipsOf(ids.v1)[0].duration, Rational(45));
    EXPECT_EQ(clipsOf(ids.a1)[0].duration, Rational(55)); // not linked this time
}

TEST_F(EditCommandsTest, DeleteClipsWithLinkedPartners) {
    const auto link = LinkId::generate();
    const auto v = add(ids.v1, Rational(0), Rational(2), Rational(), link);
    add(ids.a1, Rational(0), Rational(2), Rational(), link);
    add(ids.v2, Rational(0), Rational(2));
    pushVerified(makeDeleteClips(project, ids.sequence, {v}));
    EXPECT_TRUE(clipsOf(ids.v1).empty());
    EXPECT_TRUE(clipsOf(ids.a1).empty());
    EXPECT_EQ(clipsOf(ids.v2).size(), 1u);
}

TEST_F(EditCommandsTest, ReplaceClipsRollsBackOnFailure) {
    add(ids.v1, Rational(0), Rational(2));
    const auto before = dump(project);
    std::vector<PlacedClip> after{{ids.v1, makeClip(ids.media, Rational(1), Rational(2))}}; // overlaps
    EXPECT_ANY_THROW(stack.push(std::make_unique<ReplaceClipsCommand>("Bad", std::vector<PlacedClip>{}, after)));
    EXPECT_EQ(dump(project), before);
}

} // namespace
