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

class UndoStackTest : public ::testing::Test {
protected:
    test::BasicProject ids;
    Project project = ids.build();
    UndoStack stack{project};

    std::unique_ptr<Command> addClipAt(Rational start, TrackId track = {}) {
        return std::make_unique<AddClipCommand>(track.isNull() ? ids.v1 : track,
                                                makeClip(ids.media, start, Rational(1)));
    }
};

TEST_F(UndoStackTest, PushExecutesAndUndoRedoRestore) {
    const auto initial = dump(project);
    stack.push(addClipAt(Rational(0)));
    const auto afterAdd = dump(project);
    EXPECT_NE(afterAdd, initial);
    EXPECT_TRUE(stack.canUndo());
    EXPECT_FALSE(stack.canRedo());
    EXPECT_EQ(stack.undoText(), "Add Clip");

    stack.undo();
    EXPECT_EQ(dump(project), initial);
    EXPECT_TRUE(stack.canRedo());
    EXPECT_EQ(stack.redoText(), "Add Clip");

    stack.redo();
    EXPECT_EQ(dump(project), afterAdd);
}

TEST_F(UndoStackTest, FailedPushChangesNothing) {
    stack.push(addClipAt(Rational(0)));
    const auto before = dump(project);
    EXPECT_THROW(stack.push(addClipAt(Rational(1, 2))), ModelError); // overlaps
    EXPECT_EQ(dump(project), before);
    EXPECT_EQ(stack.count(), 1u);
    EXPECT_THROW(stack.push(nullptr), std::invalid_argument);
}

TEST_F(UndoStackTest, NewPushDiscardsRedoHistory) {
    stack.push(addClipAt(Rational(0)));
    stack.push(addClipAt(Rational(1)));
    stack.undo();
    EXPECT_TRUE(stack.canRedo());
    stack.push(addClipAt(Rational(5)));
    EXPECT_FALSE(stack.canRedo());
    EXPECT_EQ(stack.count(), 2u);
}

TEST_F(UndoStackTest, UndoRedoAtEndsAreNoOps) {
    stack.undo();
    stack.redo();
    EXPECT_EQ(stack.index(), 0u);
    EXPECT_EQ(stack.undoText(), "");
    EXPECT_EQ(stack.redoText(), "");
}

TEST_F(UndoStackTest, MacroIsOneStep) {
    const auto initial = dump(project);
    stack.beginMacro("Paste");
    stack.push(addClipAt(Rational(0)));
    stack.push(addClipAt(Rational(1)));
    EXPECT_FALSE(stack.canUndo()); // not while a macro is open
    stack.endMacro();
    EXPECT_EQ(stack.count(), 1u);
    EXPECT_EQ(stack.undoText(), "Paste");
    const auto after = dump(project);
    stack.undo();
    EXPECT_EQ(dump(project), initial);
    stack.redo();
    EXPECT_EQ(dump(project), after);
}

TEST_F(UndoStackTest, NestedMacrosCollapseIntoOuter) {
    stack.beginMacro("Outer");
    stack.push(addClipAt(Rational(0)));
    stack.beginMacro("Inner");
    stack.push(addClipAt(Rational(1)));
    stack.endMacro();
    stack.endMacro();
    EXPECT_EQ(stack.count(), 1u);
    stack.undo();
    EXPECT_TRUE(project.sequences()[0].videoTracks[0].clips.empty());
}

TEST_F(UndoStackTest, EmptyMacroIsDiscarded) {
    stack.beginMacro("Nothing");
    stack.endMacro();
    EXPECT_EQ(stack.count(), 0u);
    EXPECT_THROW(stack.endMacro(), std::logic_error);
    EXPECT_THROW(stack.abortMacro(), std::logic_error);
}

TEST_F(UndoStackTest, AbortMacroRollsBack) {
    const auto initial = dump(project);
    stack.beginMacro("Broken");
    stack.push(addClipAt(Rational(0)));
    stack.push(addClipAt(Rational(1)));
    stack.abortMacro();
    EXPECT_EQ(dump(project), initial);
    EXPECT_EQ(stack.count(), 0u);
}

TEST_F(UndoStackTest, MacroGuardAbortsOnException) {
    const auto initial = dump(project);
    try {
        MacroGuard guard(stack, "Compound");
        stack.push(addClipAt(Rational(0)));
        stack.push(addClipAt(Rational(1, 2))); // overlaps: throws
        guard.commit();
    } catch (const ModelError&) {
    }
    EXPECT_EQ(dump(project), initial);
    EXPECT_FALSE(stack.inMacro());
    EXPECT_EQ(stack.count(), 0u);
}

TEST_F(UndoStackTest, MacroCommandRollsBackFailedChild) {
    const auto initial = dump(project);
    std::vector<std::unique_ptr<Command>> children;
    children.push_back(addClipAt(Rational(0)));
    children.push_back(addClipAt(Rational(1)));
    children.push_back(addClipAt(Rational(1, 2))); // conflicts with the first
    EXPECT_THROW(stack.push(std::make_unique<MacroCommand>("Batch", std::move(children))), ModelError);
    EXPECT_EQ(dump(project), initial);
}

TEST_F(UndoStackTest, ConsecutiveMovesOfSameClipMerge) {
    auto add = std::make_unique<AddClipCommand>(ids.v1, makeClip(ids.media, Rational(0), Rational(1)));
    const auto clipId = add->clipId();
    stack.push(std::move(add));
    const auto afterAdd = dump(project);
    for (int i = 1; i <= 10; ++i) {
        stack.push(std::make_unique<MoveClipCommand>(clipId, ids.v1, Rational(i), GestureId{1}));
    }
    EXPECT_EQ(stack.count(), 2u);
    EXPECT_EQ(project.findClip(clipId).clip->start, Rational(10));
    stack.undo();
    EXPECT_EQ(dump(project), afterAdd);
    stack.redo();
    EXPECT_EQ(project.findClip(clipId).clip->start, Rational(10));
}

TEST_F(UndoStackTest, SeparateGesturesDoNotMerge) {
    auto add = std::make_unique<AddClipCommand>(ids.v1, makeClip(ids.media, Rational(0), Rational(1)));
    const auto clipId = add->clipId();
    stack.push(std::move(add));
    stack.push(std::make_unique<MoveClipCommand>(clipId, ids.v1, Rational(1), GestureId{1}));
    stack.push(std::make_unique<MoveClipCommand>(clipId, ids.v1, Rational(2), GestureId{2}));
    stack.push(std::make_unique<MoveClipCommand>(clipId, ids.v1, Rational(3)));
    stack.push(std::make_unique<MoveClipCommand>(clipId, ids.v1, Rational(4)));
    EXPECT_EQ(stack.count(), 5u);
}

TEST_F(UndoStackTest, MovesOfDifferentClipsDoNotMerge) {
    auto a = std::make_unique<AddClipCommand>(ids.v1, makeClip(ids.media, Rational(0), Rational(1)));
    auto b = std::make_unique<AddClipCommand>(ids.v2, makeClip(ids.media, Rational(0), Rational(1)));
    const auto idA = a->clipId();
    const auto idB = b->clipId();
    stack.push(std::move(a));
    stack.push(std::move(b));
    stack.push(std::make_unique<MoveClipCommand>(idA, ids.v1, Rational(3), GestureId{1}));
    stack.push(std::make_unique<MoveClipCommand>(idB, ids.v2, Rational(3), GestureId{1}));
    EXPECT_EQ(stack.count(), 4u);
}

TEST_F(UndoStackTest, DoesNotMergeAcrossCleanState) {
    auto add = std::make_unique<AddClipCommand>(ids.v1, makeClip(ids.media, Rational(0), Rational(1)));
    const auto clipId = add->clipId();
    stack.push(std::move(add));
    stack.push(std::make_unique<MoveClipCommand>(clipId, ids.v1, Rational(1), GestureId{1}));
    stack.setClean();
    stack.push(std::make_unique<MoveClipCommand>(clipId, ids.v1, Rational(2), GestureId{1}));
    EXPECT_EQ(stack.count(), 3u);
    stack.undo();
    EXPECT_TRUE(stack.isClean());
}

TEST_F(UndoStackTest, CleanStateTracking) {
    EXPECT_TRUE(stack.isClean());
    stack.push(addClipAt(Rational(0)));
    EXPECT_FALSE(stack.isClean());
    stack.setClean();
    EXPECT_TRUE(stack.isClean());
    stack.push(addClipAt(Rational(1)));
    EXPECT_FALSE(stack.isClean());
    stack.undo();
    EXPECT_TRUE(stack.isClean());
    stack.undo();
    EXPECT_FALSE(stack.isClean());
    // Branch off below the clean point: the saved state becomes unreachable.
    stack.push(addClipAt(Rational(5)));
    stack.undo();
    EXPECT_FALSE(stack.isClean());
}

TEST_F(UndoStackTest, MarkCleanAtForAsyncSaves) {
    stack.push(addClipAt(Rational(0)));
    const auto point = stack.currentPoint();
    stack.push(addClipAt(Rational(1))); // edit while "saving"
    EXPECT_TRUE(stack.markCleanAt(point));
    EXPECT_FALSE(stack.isClean());
    stack.undo();
    EXPECT_TRUE(stack.isClean());
}

TEST_F(UndoStackTest, MarkCleanAtRejectsRewrittenHistory) {
    stack.push(addClipAt(Rational(0)));
    stack.push(addClipAt(Rational(1)));
    const auto point = stack.currentPoint();
    stack.undo();
    stack.push(addClipAt(Rational(5))); // same index, different state
    EXPECT_FALSE(stack.markCleanAt(point));
    EXPECT_FALSE(stack.isClean());
}

TEST_F(UndoStackTest, MarkCleanAtRejectsMergedTop) {
    auto add = std::make_unique<AddClipCommand>(ids.v1, makeClip(ids.media, Rational(0), Rational(1)));
    const auto clipId = add->clipId();
    stack.push(std::move(add));
    stack.push(std::make_unique<MoveClipCommand>(clipId, ids.v1, Rational(1), GestureId{1}));
    const auto point = stack.currentPoint();
    stack.push(std::make_unique<MoveClipCommand>(clipId, ids.v1, Rational(2), GestureId{1})); // merges into top
    EXPECT_FALSE(stack.markCleanAt(point));
}

TEST_F(UndoStackTest, ClearResetsHistory) {
    stack.push(addClipAt(Rational(0)));
    const auto point = stack.currentPoint();
    stack.clear();
    EXPECT_EQ(stack.count(), 0u);
    EXPECT_TRUE(stack.isClean());
    EXPECT_FALSE(stack.canUndo());
    EXPECT_FALSE(stack.markCleanAt(point));
    EXPECT_EQ(project.sequences()[0].videoTracks[0].clips.size(), 1u); // project untouched
}

TEST_F(UndoStackTest, ChangedCallbackFires) {
    int calls = 0;
    stack.setChangedCallback([&] { ++calls; });
    stack.push(addClipAt(Rational(0)));
    stack.undo();
    stack.redo();
    stack.setClean();
    EXPECT_EQ(calls, 4);
}

} // namespace
