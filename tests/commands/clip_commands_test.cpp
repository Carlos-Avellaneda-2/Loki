#include "commands/clip_commands.h"
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

class ClipCommandsTest : public ::testing::Test {
protected:
    test::BasicProject ids;
    Project project = ids.build();
    UndoStack stack{project};
    test::RecordingObserver observer;

    void SetUp() override { project.addObserver(&observer); }
    void TearDown() override { project.removeObserver(&observer); }

    ClipId add(TrackId track, Rational start, Rational duration, Rational sourceIn = {}) {
        auto cmd = std::make_unique<AddClipCommand>(track, makeClip(ids.media, start, duration, sourceIn));
        const auto id = cmd->clipId();
        stack.push(std::move(cmd));
        return id;
    }

    void pushAndVerifyRoundTrip(std::unique_ptr<Command> command) {
        const auto before = dump(project);
        stack.push(std::move(command));
        const auto after = dump(project);
        stack.undo();
        ASSERT_EQ(dump(project), before);
        stack.redo();
        ASSERT_EQ(dump(project), after);
    }

    void expectRejected(std::unique_ptr<Command> command) {
        const auto before = dump(project);
        const auto count = stack.count();
        EXPECT_ANY_THROW(stack.push(std::move(command)));
        EXPECT_EQ(dump(project), before);
        EXPECT_EQ(stack.count(), count);
    }

    const Clip& clip(ClipId id) { return *project.findClip(id).clip; }
};

TEST_F(ClipCommandsTest, AddKeepsTrackSorted) {
    add(ids.v1, Rational(10), Rational(1));
    add(ids.v1, Rational(0), Rational(1));
    add(ids.v1, Rational(5), Rational(1));
    const auto& clips = project.findTrack(ids.v1).track->clips;
    ASSERT_EQ(clips.size(), 3u);
    EXPECT_EQ(clips[0].start, Rational(0));
    EXPECT_EQ(clips[1].start, Rational(5));
    EXPECT_EQ(clips[2].start, Rational(10));
    EXPECT_EQ(observer.count(ChangeKind::ClipAdded), 3u);
}

TEST_F(ClipCommandsTest, AdjacentClipsDoNotOverlap) {
    add(ids.v1, Rational(0), Rational(1001, 30000));
    EXPECT_NO_THROW(add(ids.v1, Rational(1001, 30000), Rational(1)));
}

TEST_F(ClipCommandsTest, AddRejections) {
    add(ids.v1, Rational(0), Rational(2));
    expectRejected(std::make_unique<AddClipCommand>(ids.v1, makeClip(ids.media, Rational(1), Rational(2))));
    expectRejected(std::make_unique<AddClipCommand>(TrackId::generate(), makeClip(ids.media, Rational(0), Rational(1))));
    expectRejected(
        std::make_unique<AddClipCommand>(ids.v2, makeClip(MediaId::generate(), Rational(0), Rational(1))));
    expectRejected(std::make_unique<AddClipCommand>(ids.v2, makeClip(ids.media, Rational(0), Rational(0))));
}

TEST_F(ClipCommandsTest, RemoveRoundTrip) {
    const auto id = add(ids.v1, Rational(3), Rational(2));
    pushAndVerifyRoundTrip(std::make_unique<RemoveClipCommand>(id));
    EXPECT_EQ(project.findClip(id).clip, nullptr);
    expectRejected(std::make_unique<RemoveClipCommand>(ClipId::generate()));
}

TEST_F(ClipCommandsTest, MoveWithinAndAcrossTracks) {
    const auto id = add(ids.v1, Rational(0), Rational(2));
    pushAndVerifyRoundTrip(std::make_unique<MoveClipCommand>(id, ids.v1, Rational(10)));
    EXPECT_EQ(clip(id).start, Rational(10));
    pushAndVerifyRoundTrip(std::make_unique<MoveClipCommand>(id, ids.v2, Rational(3)));
    EXPECT_EQ(project.findClip(id).track->id, ids.v2);
    EXPECT_EQ(clip(id).start, Rational(3));
}

TEST_F(ClipCommandsTest, MoveRejections) {
    const auto a = add(ids.v1, Rational(0), Rational(2));
    add(ids.v2, Rational(0), Rational(2));
    expectRejected(std::make_unique<MoveClipCommand>(a, ids.v2, Rational(1)));      // overlap on target
    expectRejected(std::make_unique<MoveClipCommand>(a, ids.a1, Rational(10)));     // video -> audio
    expectRejected(std::make_unique<MoveClipCommand>(a, ids.v1, Rational(-1)));     // negative start
    expectRejected(std::make_unique<MoveClipCommand>(a, TrackId::generate(), Rational(0)));
    // Failed cross-track move must leave the clip on its original track.
    EXPECT_EQ(project.findClip(a).track->id, ids.v1);
}

TEST_F(ClipCommandsTest, LockedTracksRejectEdits) {
    const auto id = add(ids.v1, Rational(0), Rational(2));
    stack.push(setTrackLocked(project, ids.v1, true));
    expectRejected(std::make_unique<AddClipCommand>(ids.v1, makeClip(ids.media, Rational(5), Rational(1))));
    expectRejected(std::make_unique<RemoveClipCommand>(id));
    expectRejected(std::make_unique<MoveClipCommand>(id, ids.v1, Rational(5)));
    expectRejected(std::make_unique<MoveClipCommand>(id, ids.v2, Rational(5)));
    expectRejected(std::make_unique<SplitClipCommand>(id, Rational(1)));
    expectRejected(renameClip(id, "x"));
    // Moving onto a locked track is rejected too.
    const auto other = add(ids.v2, Rational(0), Rational(1));
    expectRejected(std::make_unique<MoveClipCommand>(other, ids.v1, Rational(10)));
    // Undo past the lock still works.
    stack.undo(); // add other
    stack.undo(); // lock
    EXPECT_NO_THROW(stack.push(renameClip(id, "ok")));
}

TEST_F(ClipCommandsTest, SplitKeepsSourceContinuity) {
    const auto id = add(ids.v1, Rational(10), Rational(6), Rational(2));
    auto split = std::make_unique<SplitClipCommand>(id, Rational(12));
    const auto rightId = split->rightClipId();
    pushAndVerifyRoundTrip(std::move(split));
    const auto& left = clip(id);
    const auto& right = clip(rightId);
    EXPECT_EQ(left.start, Rational(10));
    EXPECT_EQ(left.duration, Rational(2));
    EXPECT_EQ(left.sourceIn, Rational(2));
    EXPECT_EQ(right.start, Rational(12));
    EXPECT_EQ(right.duration, Rational(4));
    EXPECT_EQ(right.sourceIn, Rational(4)); // continues where the left part ends
    EXPECT_EQ(left.sourceOut(), right.sourceIn);
    EXPECT_EQ(right.media, left.media);
}

TEST_F(ClipCommandsTest, SplitPreservesKeyframedValues) {
    const auto id = add(ids.v1, Rational(0), Rational(10));
    auto effect = test::makeOpacityEffect();
    const auto effectId = effect.id;
    stack.push(addEffect(id, effect));
    stack.push(setKeyframe(id, effectId, "opacity", Keyframe{Rational(0), 0.0}));
    stack.push(setKeyframe(id, effectId, "opacity", Keyframe{Rational(10), 100.0}));
    auto split = std::make_unique<SplitClipCommand>(id, Rational(4));
    const auto rightId = split->rightClipId();
    stack.push(std::move(split));
    const auto valueAt = [&](ClipId clipId, Rational seqTime) {
        const auto& c = clip(clipId);
        return c.effects[0].findParameter("opacity")->valueAt(c.toSourceTime(seqTime));
    };
    EXPECT_DOUBLE_EQ(valueAt(id, Rational(2)), 20.0);
    EXPECT_DOUBLE_EQ(valueAt(rightId, Rational(6)), 60.0);
}

TEST_F(ClipCommandsTest, SplitRejectsPointsOutsideClip) {
    const auto id = add(ids.v1, Rational(10), Rational(6));
    expectRejected(std::make_unique<SplitClipCommand>(id, Rational(10)));
    expectRejected(std::make_unique<SplitClipCommand>(id, Rational(16)));
    expectRejected(std::make_unique<SplitClipCommand>(id, Rational(20)));
    expectRejected(std::make_unique<SplitClipCommand>(ClipId::generate(), Rational(12)));
}

TEST_F(ClipCommandsTest, TrimMergesWhileDragging) {
    const auto id = add(ids.v1, Rational(0), Rational(10));
    const auto before = dump(project);
    for (int i = 1; i <= 5; ++i) {
        stack.push(trimClip(id, Rational(i), Rational(i), Rational(10 - i), GestureId{7}));
    }
    EXPECT_EQ(stack.undoText(), "Trim Clip");
    EXPECT_EQ(clip(id).start, Rational(5));
    EXPECT_EQ(clip(id).sourceIn, Rational(5));
    stack.undo();
    EXPECT_EQ(dump(project), before);
    expectRejected(trimClip(id, Rational(0), Rational(-1), Rational(1)));
    expectRejected(trimClip(id, Rational(0), Rational(0), Rational(0)));
}

TEST_F(ClipCommandsTest, TrimRejectsOverlapWithNeighbour) {
    const auto a = add(ids.v1, Rational(0), Rational(5));
    add(ids.v1, Rational(5), Rational(5));
    expectRejected(trimClip(a, Rational(0), Rational(0), Rational(6)));
}

TEST_F(ClipCommandsTest, EnableAndRename) {
    const auto id = add(ids.v1, Rational(0), Rational(1));
    pushAndVerifyRoundTrip(setClipEnabled(id, false));
    EXPECT_FALSE(clip(id).enabled);
    pushAndVerifyRoundTrip(renameClip(id, "Take 3"));
    EXPECT_EQ(clip(id).name, "Take 3");
}

TEST_F(ClipCommandsTest, EffectsParametersAndKeyframes) {
    const auto id = add(ids.v1, Rational(0), Rational(10));
    auto effect = test::makeOpacityEffect();
    const auto effectId = effect.id;
    pushAndVerifyRoundTrip(addEffect(id, effect));
    ASSERT_EQ(clip(id).effects.size(), 1u);

    pushAndVerifyRoundTrip(setEffectEnabled(id, effectId, false));
    EXPECT_FALSE(clip(id).effects[0].enabled);

    // Slider drag: one undo step.
    const auto beforeDrag = dump(project);
    for (double v = 90; v >= 50; v -= 10) {
        stack.push(setParameterValue(id, effectId, "opacity", v, GestureId{9}));
    }
    EXPECT_DOUBLE_EQ(clip(id).effects[0].findParameter("opacity")->value, 50.0);
    stack.undo();
    EXPECT_EQ(dump(project), beforeDrag);
    stack.redo();

    pushAndVerifyRoundTrip(setKeyframe(id, effectId, "opacity", Keyframe{Rational(2), 10.0}));
    pushAndVerifyRoundTrip(setKeyframe(id, effectId, "opacity", Keyframe{Rational(1), 5.0}));
    pushAndVerifyRoundTrip(setKeyframe(id, effectId, "opacity", Keyframe{Rational(2), 20.0})); // replace
    const auto& keys = clip(id).effects[0].findParameter("opacity")->keyframes;
    ASSERT_EQ(keys.size(), 2u);
    EXPECT_EQ(keys[0].time, Rational(1));
    EXPECT_DOUBLE_EQ(keys[1].value, 20.0);

    pushAndVerifyRoundTrip(removeKeyframe(id, effectId, "opacity", Rational(1)));
    EXPECT_EQ(clip(id).effects[0].findParameter("opacity")->keyframes.size(), 1u);

    expectRejected(removeKeyframe(id, effectId, "opacity", Rational(7)));
    expectRejected(setParameterValue(id, effectId, "missing", 1.0));
    expectRejected(setParameterValue(id, EffectId::generate(), "opacity", 1.0));
    Keyframe bad{Rational(3), 1.0};
    bad.easeIn = 2.0;
    expectRejected(setKeyframe(id, effectId, "opacity", bad));

    pushAndVerifyRoundTrip(removeEffect(id, effectId));
    EXPECT_TRUE(clip(id).effects.empty());
    expectRejected(removeEffect(id, effectId));
}

TEST_F(ClipCommandsTest, ParameterEditsOnDifferentKeysDoNotMerge) {
    const auto id = add(ids.v1, Rational(0), Rational(10));
    auto effect = test::makeOpacityEffect();
    const auto effectId = effect.id;
    stack.push(addEffect(id, effect));
    const auto count = stack.count();
    stack.push(setParameterValue(id, effectId, "opacity", 10, GestureId{3}));
    stack.push(setParameterValue(id, effectId, "blendMode", 1, GestureId{3}));
    EXPECT_EQ(stack.count(), count + 2);
    // Without a gesture, repeated edits of the same parameter stay separate steps.
    stack.push(setParameterValue(id, effectId, "opacity", 20));
    stack.push(setParameterValue(id, effectId, "opacity", 30));
    EXPECT_EQ(stack.count(), count + 4);
}

} // namespace
