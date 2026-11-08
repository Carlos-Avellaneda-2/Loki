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
using loki::test::makeMedia;

class ProjectCommandsTest : public ::testing::Test {
protected:
    test::BasicProject ids;
    Project project = ids.build();
    UndoStack stack{project};
    test::RecordingObserver observer;

    void SetUp() override { project.addObserver(&observer); }
    void TearDown() override { project.removeObserver(&observer); }

    // Pushes, then checks undo restores `before` exactly and redo restores the result.
    void pushAndVerifyRoundTrip(std::unique_ptr<Command> command) {
        const auto before = dump(project);
        stack.push(std::move(command));
        const auto after = dump(project);
        stack.undo();
        ASSERT_EQ(dump(project), before);
        stack.redo();
        ASSERT_EQ(dump(project), after);
    }
};

TEST_F(ProjectCommandsTest, AddMediaRoundTripAndEvents) {
    auto media = makeMedia("b-roll");
    const auto id = media.id;
    pushAndVerifyRoundTrip(std::make_unique<AddMediaCommand>(media));
    ASSERT_NE(project.findMedia(id), nullptr);
    EXPECT_EQ(observer.count(ChangeKind::MediaAdded), 2u); // redo + redo-after-undo
    EXPECT_EQ(observer.count(ChangeKind::MediaRemoved), 1u);
}

TEST_F(ProjectCommandsTest, AddMediaGeneratesIdWhenMissing) {
    auto media = makeMedia("x");
    media.id = MediaId();
    auto cmd = std::make_unique<AddMediaCommand>(media);
    const auto id = cmd->mediaId();
    EXPECT_FALSE(id.isNull());
    stack.push(std::move(cmd));
    EXPECT_NE(project.findMedia(id), nullptr);
}

TEST_F(ProjectCommandsTest, RemoveMediaRemovesItsClipsAndUndoRestoresThem) {
    stack.push(std::make_unique<AddClipCommand>(ids.v1, makeClip(ids.media, Rational(0), Rational(2))));
    stack.push(std::make_unique<AddClipCommand>(ids.a1, makeClip(ids.media, Rational(0), Rational(2))));
    stack.push(std::make_unique<AddClipCommand>(ids.v1, makeClip(ids.media, Rational(5), Rational(2))));
    pushAndVerifyRoundTrip(std::make_unique<RemoveMediaCommand>(ids.media));
    EXPECT_EQ(project.findMedia(ids.media), nullptr);
    EXPECT_TRUE(project.clipsUsingMedia(ids.media).empty());
    EXPECT_TRUE(project.validate().empty());
}

TEST_F(ProjectCommandsTest, RemoveUnknownMediaFails) {
    EXPECT_THROW(stack.push(std::make_unique<RemoveMediaCommand>(MediaId::generate())), CommandError);
}

TEST_F(ProjectCommandsTest, RenameAndMoveMedia) {
    pushAndVerifyRoundTrip(renameMedia(ids.media, "Interview A"));
    EXPECT_EQ(project.findMedia(ids.media)->name, "Interview A");
    auto bin = std::make_unique<AddBinCommand>(Bin{{}, "Footage", {}});
    const auto binId = bin->binId();
    stack.push(std::move(bin));
    pushAndVerifyRoundTrip(moveMediaToBin(ids.media, binId));
    EXPECT_EQ(project.findMedia(ids.media)->bin, binId);
    EXPECT_THROW(stack.push(moveMediaToBin(ids.media, BinId::generate())), ModelError);
}

TEST_F(ProjectCommandsTest, BinsCreateRenameMoveAndRejectCycles) {
    auto a = std::make_unique<AddBinCommand>(Bin{{}, "A", {}});
    const auto idA = a->binId();
    pushAndVerifyRoundTrip(std::move(a));
    auto b = std::make_unique<AddBinCommand>(Bin{{}, "B", idA});
    const auto idB = b->binId();
    stack.push(std::move(b));

    pushAndVerifyRoundTrip(renameBin(idA, "Footage"));
    EXPECT_EQ(project.findBin(idA)->name, "Footage");

    EXPECT_THROW(stack.push(moveBin(idA, idB)), ModelError); // A into its own child
    EXPECT_THROW(stack.push(moveBin(idA, idA)), ModelError);
    pushAndVerifyRoundTrip(moveBin(idB, BinId()));
    EXPECT_TRUE(project.findBin(idB)->parent.isNull());
}

TEST_F(ProjectCommandsTest, RemoveNonEmptyBinNeedsRecursiveRemove) {
    auto a = std::make_unique<AddBinCommand>(Bin{{}, "A", {}});
    const auto idA = a->binId();
    stack.push(std::move(a));
    auto b = std::make_unique<AddBinCommand>(Bin{{}, "B", idA});
    const auto idB = b->binId();
    stack.push(std::move(b));
    stack.push(moveMediaToBin(ids.media, idB));
    stack.push(std::make_unique<AddClipCommand>(ids.v1, makeClip(ids.media, Rational(0), Rational(2))));

    EXPECT_THROW(stack.push(std::make_unique<RemoveBinCommand>(idA)), ModelError);

    pushAndVerifyRoundTrip(removeBinRecursive(project, idA));
    EXPECT_EQ(project.findBin(idA), nullptr);
    EXPECT_EQ(project.findBin(idB), nullptr);
    EXPECT_EQ(project.findMedia(ids.media), nullptr);
    EXPECT_TRUE(project.sequences()[0].videoTracks[0].clips.empty());
    EXPECT_EQ(stack.undoText(), "Remove Bin");
}

TEST_F(ProjectCommandsTest, Sequences) {
    auto add = std::make_unique<AddSequenceCommand>(makeDefaultSequence("Second"));
    const auto id = add->sequenceId();
    pushAndVerifyRoundTrip(std::move(add));
    ASSERT_NE(project.findSequence(id), nullptr);
    pushAndVerifyRoundTrip(std::make_unique<RenameSequenceCommand>(id, "Renamed"));
    EXPECT_EQ(project.findSequence(id)->name, "Renamed");

    SequenceSettings settings;
    settings.frameRate = Rational(24000, 1001);
    settings.width = 1280;
    settings.height = 720;
    pushAndVerifyRoundTrip(std::make_unique<SetSequenceSettingsCommand>(id, settings));
    EXPECT_EQ(project.findSequence(id)->settings, settings);

    settings.sampleRate = 0;
    EXPECT_THROW(stack.push(std::make_unique<SetSequenceSettingsCommand>(id, settings)), ModelError);

    pushAndVerifyRoundTrip(std::make_unique<RemoveSequenceCommand>(id));
    EXPECT_EQ(project.findSequence(id), nullptr);
    EXPECT_THROW(stack.push(std::make_unique<RenameSequenceCommand>(id, "x")), CommandError);
}

TEST_F(ProjectCommandsTest, AddTrackNamesAndPositions) {
    auto top = std::make_unique<AddTrackCommand>(ids.sequence, TrackKind::Video);
    const auto topId = top->trackId();
    pushAndVerifyRoundTrip(std::move(top));
    const auto& seq = *project.findSequence(ids.sequence);
    ASSERT_EQ(seq.videoTracks.size(), 3u);
    EXPECT_EQ(seq.videoTracks[2].id, topId);
    EXPECT_EQ(seq.videoTracks[2].state.name, "V3");

    auto bottom = std::make_unique<AddTrackCommand>(ids.sequence, TrackKind::Audio, 0, "Music");
    const auto bottomId = bottom->trackId();
    stack.push(std::move(bottom));
    EXPECT_EQ(project.findSequence(ids.sequence)->audioTracks[0].id, bottomId);
    EXPECT_EQ(project.findSequence(ids.sequence)->audioTracks[0].state.name, "Music");
    EXPECT_EQ(project.findSequence(ids.sequence)->audioTracks[0].kind, TrackKind::Audio);

    EXPECT_THROW(stack.push(std::make_unique<AddTrackCommand>(SequenceId::generate(), TrackKind::Video)),
                 CommandError);
}

TEST_F(ProjectCommandsTest, RemoveTrackRestoresClipsAndPosition) {
    stack.push(std::make_unique<AddClipCommand>(ids.v1, makeClip(ids.media, Rational(0), Rational(2))));
    pushAndVerifyRoundTrip(std::make_unique<RemoveTrackCommand>(ids.v1));
    EXPECT_EQ(project.findSequence(ids.sequence)->videoTracks.size(), 1u);
    stack.undo();
    EXPECT_EQ(project.findSequence(ids.sequence)->videoTracks[0].id, ids.v1);
    EXPECT_EQ(project.findSequence(ids.sequence)->videoTracks[0].clips.size(), 1u);
}

TEST_F(ProjectCommandsTest, TrackStateFactories) {
    pushAndVerifyRoundTrip(setTrackLocked(project, ids.v1, true));
    EXPECT_TRUE(project.findTrack(ids.v1).track->state.locked);
    EXPECT_EQ(stack.undoText(), "Lock Track");
    pushAndVerifyRoundTrip(setTrackHidden(project, ids.v2, true));
    EXPECT_TRUE(project.findTrack(ids.v2).track->state.hidden);
    pushAndVerifyRoundTrip(setTrackMuted(project, ids.a1, true));
    EXPECT_TRUE(project.findTrack(ids.a1).track->state.muted);
    pushAndVerifyRoundTrip(renameTrack(project, ids.a1, "Dialogue"));
    EXPECT_EQ(project.findTrack(ids.a1).track->state.name, "Dialogue");
    EXPECT_TRUE(project.findTrack(ids.a1).track->state.muted); // other flags preserved
    EXPECT_GT(observer.count(ChangeKind::TrackChanged), 0u);
    EXPECT_THROW((void)setTrackLocked(project, TrackId::generate(), true), CommandError);
}

} // namespace
