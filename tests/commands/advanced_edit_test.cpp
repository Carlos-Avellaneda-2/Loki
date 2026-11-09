#include "commands/edit_commands.h"
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
class AdvancedEditTest : public ::testing::Test {
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
    const std::vector<Clip>& clipsOf(TrackId track) { return project.findTrack(track).track->clips; }
    const Clip& clip(ClipId id) { return *project.findClip(id).clip; }
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

TEST_F(AdvancedEditTest, RippleTrimOutPushesFollowingClipsOnAllUnlockedTracks) {
    const auto a = add(ids.v1, Rational(0), Rational(4), Rational(10));
    const auto b = add(ids.v1, Rational(4), Rational(2));
    const auto c = add(ids.a1, Rational(6), Rational(1));
    pushVerified(makeRippleTrim(project, ids.sequence, a, ClipEdge::Out, Rational(2)));
    EXPECT_EQ(clip(a).duration, Rational(6));
    EXPECT_EQ(clip(b).start, Rational(6));
    EXPECT_EQ(clip(c).start, Rational(8));
    pushVerified(makeRippleTrim(project, ids.sequence, a, ClipEdge::Out, Rational(-3)));
    EXPECT_EQ(clip(b).start, Rational(3)); // gap closed: b follows a's new end
}

TEST_F(AdvancedEditTest, RippleTrimInKeepsStartAndPullsTheRest) {
    const auto a = add(ids.v1, Rational(2), Rational(4), Rational(10));
    const auto b = add(ids.v1, Rational(6), Rational(2));
    pushVerified(makeRippleTrim(project, ids.sequence, a, ClipEdge::In, Rational(1)));
    EXPECT_EQ(clip(a).start, Rational(2));
    EXPECT_EQ(clip(a).sourceIn, Rational(11));
    EXPECT_EQ(clip(a).duration, Rational(3));
    EXPECT_EQ(clip(b).start, Rational(5));
}

TEST_F(AdvancedEditTest, RollMovesTheEditPointBetweenNeighbours) {
    const auto a = add(ids.v1, Rational(0), Rational(4), Rational(10));
    const auto b = add(ids.v1, Rational(4), Rational(4), Rational(20));
    pushVerified(makeRollEdit(project, ids.sequence, a, ClipEdge::Out, Rational(1)));
    EXPECT_EQ(clip(a).duration, Rational(5));
    EXPECT_EQ(clip(b).start, Rational(5));
    EXPECT_EQ(clip(b).sourceIn, Rational(21));
    EXPECT_EQ(clip(b).end(), Rational(8)); // total unchanged
}

TEST_F(AdvancedEditTest, RollIsClampedByTheRightClipSource) {
    const auto a = add(ids.v1, Rational(0), Rational(10), Rational(0));
    const auto b = add(ids.v1, Rational(10), Rational(4), Rational(2));
    pushVerified(makeRollEdit(project, ids.sequence, b, ClipEdge::In, Rational(-100)));
    EXPECT_EQ(clip(b).sourceIn, Rational());
    EXPECT_EQ(clip(b).start, Rational(8));
    EXPECT_EQ(clip(a).end(), Rational(8));
}

TEST_F(AdvancedEditTest, RollIsClampedByTheLeftClipLength) {
    const auto a = add(ids.v1, Rational(0), Rational(2), Rational(0));
    const auto b = add(ids.v1, Rational(2), Rational(4), Rational(30));
    pushVerified(makeRollEdit(project, ids.sequence, b, ClipEdge::In, Rational(-10)));
    EXPECT_EQ(clip(a).duration, Rational(1, 30)); // left keeps one frame
    EXPECT_EQ(clip(b).start, Rational(1, 30));
    EXPECT_EQ(clip(b).end(), Rational(6));
}

TEST_F(AdvancedEditTest, SlipChangesSourceOnlyAndRespectsMediaBounds) {
    const auto link = LinkId::generate();
    const auto v = add(ids.v1, Rational(5), Rational(10), Rational(20), link);
    add(ids.a1, Rational(5), Rational(10), Rational(20), link);
    pushVerified(makeSlipEdit(project, ids.sequence, v, Rational(3)));
    EXPECT_EQ(clip(v).sourceIn, Rational(23));
    EXPECT_EQ(clip(v).start, Rational(5));
    EXPECT_EQ(clipsOf(ids.a1)[0].sourceIn, Rational(23)); // linked audio slips too
    pushVerified(makeSlipEdit(project, ids.sequence, v, Rational(100)));
    EXPECT_EQ(clip(v).sourceOut(), Rational(60));
    pushVerified(makeSlipEdit(project, ids.sequence, v, Rational(-100)));
    EXPECT_EQ(clip(v).sourceIn, Rational());
}

TEST_F(AdvancedEditTest, SlideAdjustsNeighbours) {
    const auto a = add(ids.v1, Rational(0), Rational(4), Rational(10));
    const auto b = add(ids.v1, Rational(4), Rational(2), Rational(30));
    const auto c = add(ids.v1, Rational(6), Rational(4), Rational(40));
    pushVerified(makeSlideEdit(project, ids.sequence, b, Rational(1)));
    EXPECT_EQ(clip(a).duration, Rational(5));
    EXPECT_EQ(clip(b).start, Rational(5));
    EXPECT_EQ(clip(b).sourceIn, Rational(30)); // content unchanged
    EXPECT_EQ(clip(c).start, Rational(7));
    EXPECT_EQ(clip(c).sourceIn, Rational(41));
    EXPECT_EQ(clip(c).end(), Rational(10));
}

TEST_F(AdvancedEditTest, SlideIntoGapIsLimitedByTheGap) {
    add(ids.v1, Rational(0), Rational(2));
    const auto b = add(ids.v1, Rational(5), Rational(2));
    pushVerified(makeSlideEdit(project, ids.sequence, b, Rational(-10)));
    EXPECT_EQ(clip(b).start, Rational(2)); // stops at the previous clip
}

TEST_F(AdvancedEditTest, RippleDeleteClosesGapsWhereTracksAreFree) {
    const auto a = add(ids.v1, Rational(0), Rational(2));
    const auto b = add(ids.v1, Rational(2), Rational(3));
    const auto c = add(ids.v1, Rational(5), Rational(1));
    const auto busy = add(ids.v2, Rational(3), Rational(1)); // V2 has content inside the range
    const auto later = add(ids.a1, Rational(10), Rational(1));
    pushVerified(makeRippleDelete(project, ids.sequence, {b}));
    EXPECT_EQ(project.findClip(b).clip, nullptr);
    EXPECT_EQ(clip(a).start, Rational(0));
    EXPECT_EQ(clip(c).start, Rational(2));     // V1 closed
    EXPECT_EQ(clip(busy).start, Rational(3));  // V2 not shifted (D14)
    EXPECT_EQ(clip(later).start, Rational(7)); // A1 free in [2,5): shifted
}

TEST_F(AdvancedEditTest, LiftAndExtractUseTheRange) {
    add(ids.v1, Rational(0), Rational(10));
    const auto a1 = add(ids.a1, Rational(8), Rational(4));
    pushVerified(makeLiftRange(project, ids.sequence, Rational(2), Rational(4)));
    ASSERT_EQ(clipsOf(ids.v1).size(), 2u);
    EXPECT_EQ(clipsOf(ids.v1)[1].start, Rational(4));
    stack.undo();
    pushVerified(makeExtractRange(project, ids.sequence, Rational(2), Rational(4)));
    ASSERT_EQ(clipsOf(ids.v1).size(), 2u);
    EXPECT_EQ(clipsOf(ids.v1)[1].start, Rational(2)); // gap closed
    EXPECT_EQ(clipsOf(ids.v1)[1].end(), Rational(8));
    EXPECT_EQ(clip(a1).start, Rational(6));
    EXPECT_THROW((void)makeLiftRange(project, ids.sequence, Rational(4), Rational(4)), CommandError);
}

TEST_F(AdvancedEditTest, LinkAndUnlink) {
    const auto v = add(ids.v1, Rational(0), Rational(2));
    const auto a = add(ids.a1, Rational(0), Rational(2));
    pushVerified(makeLinkClips(project, ids.sequence, {v, a}));
    EXPECT_FALSE(clip(v).link.isNull());
    EXPECT_EQ(clip(v).link, clip(a).link);
    pushVerified(makeUnlinkClips(project, ids.sequence, {v}));
    EXPECT_TRUE(clip(v).link.isNull());
    EXPECT_TRUE(clip(a).link.isNull());
    EXPECT_THROW((void)makeLinkClips(project, ids.sequence, {v}), CommandError);
}

TEST_F(AdvancedEditTest, SequenceMarksAreUndoableAndSerialized) {
    pushVerified(setSequenceMarkIn(project, ids.sequence, Rational(2)));
    pushVerified(setSequenceMarkOut(project, ids.sequence, Rational(5)));
    const auto* seq = project.findSequence(ids.sequence);
    EXPECT_EQ(seq->markIn, Rational(2));
    EXPECT_EQ(seq->markOut, Rational(5));
    // In past Out clears Out (Premiere).
    pushVerified(setSequenceMarkIn(project, ids.sequence, Rational(6)));
    EXPECT_FALSE(project.findSequence(ids.sequence)->markOut.has_value());
    // JSON round trip keeps the marks; files without them still load (D13).
    const auto loaded = parseProject(serializeProject(project));
    EXPECT_EQ(loaded.findSequence(ids.sequence)->markIn, Rational(6));
    EXPECT_EQ(loaded, project);
}

} // namespace
