#include "commands/clip_commands.h"
#include "commands/edit_commands.h"
#include "commands/effect_commands.h"
#include "commands/project_commands.h"
#include "commands/undo_stack.h"
#include "model/effect_registry.h"
#include "model/project_json.h"

#include "model_fixtures.h"

#include <gtest/gtest.h>

namespace {

using namespace loki;
using namespace loki::model;
using namespace loki::commands;
using loki::test::dump;
using loki::test::makeClip;

class AudioCommandsTest : public ::testing::Test {
protected:
    test::BasicProject ids; // V1, V2, A1 at 30 fps; media 60 s
    Project project = ids.build();
    UndoStack stack{project};

    ClipId add(TrackId track, Rational start, Rational duration, LinkId link = {}) {
        auto clip = makeClip(ids.media, start, duration);
        clip.link = link;
        auto cmd = std::make_unique<AddClipCommand>(track, clip);
        const auto id = cmd->clipId();
        stack.push(std::move(cmd));
        return id;
    }
    const Clip& clip(ClipId id) { return *project.findClip(id).clip; }
    const TrackState& state(TrackId id) { return project.findTrack(id).track->state; }
    const std::vector<Clip>& clipsOf(TrackId track) { return project.findTrack(track).track->clips; }
};

TEST_F(AudioCommandsTest, TrackFaderDragIsOneUndoStep) {
    const auto before = stack.count();
    stack.push(setTrackVolume(project, ids.a1, -3, 7));
    stack.push(setTrackVolume(project, ids.a1, -9, 7));
    stack.push(setTrackPan(project, ids.a1, -40, 8));
    stack.push(setTrackPan(project, ids.a1, 35, 8));
    EXPECT_EQ(stack.count(), before + 2);
    EXPECT_EQ(state(ids.a1).volumeDb, -9);
    EXPECT_EQ(state(ids.a1).pan, 35);
    stack.undo();
    EXPECT_EQ(state(ids.a1).pan, 0);
    stack.undo();
    EXPECT_EQ(state(ids.a1).volumeDb, 0);
    EXPECT_THROW((void)setTrackPan(project, ids.a1, 101), CommandError);
}

TEST_F(AudioCommandsTest, SoloAndMasterVolume) {
    stack.push(setTrackSolo(project, ids.a1, true));
    EXPECT_TRUE(state(ids.a1).solo);
    stack.push(std::make_unique<SetMasterVolumeCommand>(ids.sequence, -4.5, 3));
    stack.push(std::make_unique<SetMasterVolumeCommand>(ids.sequence, -2.0, 3));
    EXPECT_EQ(project.findSequence(ids.sequence)->masterVolumeDb, -2.0);
    stack.undo();
    EXPECT_EQ(project.findSequence(ids.sequence)->masterVolumeDb, 0.0);
    stack.undo();
    EXPECT_FALSE(state(ids.a1).solo);
}

TEST_F(AudioCommandsTest, FadesAreClampedToTheClip) {
    const auto a = add(ids.a1, Rational(0), Rational(4));
    stack.push(setClipFades(a, Rational(1), Rational(9), 5));
    stack.push(setClipFades(a, Rational(2), Rational(9), 5)); // same handle drag
    EXPECT_EQ(clip(a).fadeIn, Rational(2));
    EXPECT_EQ(clip(a).fadeOut, Rational(4));
    stack.undo();
    EXPECT_EQ(clip(a).fadeIn, Rational());
    EXPECT_THROW((void)setClipFades(a, Rational(-1), Rational()), CommandError);
}

TEST_F(AudioCommandsTest, MixSettingsRoundTripAndOldFilesStillLoad) {
    const auto a = add(ids.a1, Rational(0), Rational(4));
    stack.push(setClipFades(a, Rational(1, 2), Rational(1, 3)));
    stack.push(setTrackVolume(project, ids.a1, -6.5));
    stack.push(setTrackPan(project, ids.a1, 25));
    stack.push(setTrackSolo(project, ids.a1, true));
    stack.push(std::make_unique<SetMasterVolumeCommand>(ids.sequence, 2.5));
    const auto json = serializeProject(project);
    EXPECT_EQ(parseProject(json), project);
    // Defaults are omitted, so v1 files written before Phase 8 are unchanged by an empty mix.
    Project plain = test::BasicProject{}.build();
    const auto plainJson = serializeProject(plain);
    for (const char* key : {"volumeDb", "\"pan\"", "solo", "masterVolumeDb", "fadeIn", "fadeOut"}) {
        EXPECT_EQ(plainJson.find(key), std::string::npos) << key;
    }
}

TEST_F(AudioCommandsTest, AudioEffectsAreFixedForAudioClips) {
    const auto* volume = findEffectDef(kVolumeEffect);
    const auto* panner = findEffectDef(kPannerEffect);
    ASSERT_NE(volume, nullptr);
    ASSERT_NE(panner, nullptr);
    EXPECT_TRUE(volume->fixed);
    EXPECT_EQ(volume->kind, TrackKind::Audio);
    EXPECT_EQ(panner->kind, TrackKind::Audio);
    EXPECT_EQ(findEffectDef(kMotionEffect)->kind, TrackKind::Video);
    const auto a = add(ids.a1, Rational(0), Rational(4));
    EXPECT_EQ(resolveAudioMix(clip(a), Rational(1)).levelDb, 0.0);
    stack.push(setEffectParameter(project, a, kVolumeEffect, "level", -12.0, Rational(0)));
    EXPECT_EQ(resolveAudioMix(clip(a), Rational(1)).levelDb, -12.0);
}

TEST_F(AudioCommandsTest, CopyPasteOverwriteKeepsLayoutAndRelinks) {
    const auto link = LinkId::generate();
    const auto v = add(ids.v1, Rational(1), Rational(2), link);
    const auto a = add(ids.a1, Rational(1), Rational(2), link);
    const auto board = copyClips(project, ids.sequence, {v}); // linked audio comes along
    ASSERT_EQ(board.items.size(), 2u);
    EXPECT_EQ(board.duration, Rational(2));
    EXPECT_EQ(board.items[0].clip.start, Rational());
    stack.push(makePaste(project, ids.sequence, board, Rational(5), {ids.v2, ids.a1}, false));
    ASSERT_EQ(clipsOf(ids.v2).size(), 1u);
    ASSERT_EQ(clipsOf(ids.a1).size(), 2u);
    const auto& pv = clipsOf(ids.v2)[0];
    const auto& pa = clipsOf(ids.a1)[1];
    EXPECT_EQ(pv.start, Rational(5));
    EXPECT_EQ(pa.start, Rational(5));
    EXPECT_NE(pv.id, v);
    EXPECT_NE(pa.id, a);
    EXPECT_FALSE(pv.link.isNull());
    EXPECT_EQ(pv.link, pa.link);
    EXPECT_NE(pv.link, link);
    // Overwrite: pasting over the original replaces that range.
    stack.push(makePaste(project, ids.sequence, board, Rational(2), {ids.v1, ids.a1}, false));
    EXPECT_EQ(clipsOf(ids.v1).size(), 2u);
    EXPECT_EQ(clipsOf(ids.v1)[0].end(), Rational(2));
    EXPECT_TRUE(project.validate().empty());
}

TEST_F(AudioCommandsTest, PasteInsertMakesRoomAndChecksTracks) {
    add(ids.v1, Rational(0), Rational(4));
    const auto board = copyClips(project, ids.sequence, {clipsOf(ids.v1)[0].id});
    stack.push(makePaste(project, ids.sequence, board, Rational(1), {ids.v1, {}}, true));
    ASSERT_EQ(clipsOf(ids.v1).size(), 3u);
    EXPECT_EQ(clipsOf(ids.v1)[1].start, Rational(1));
    EXPECT_EQ(clipsOf(ids.v1)[2].start, Rational(5)); // the split rest moved right
    // Two video tracks copied, pasted from V2: there is no V3.
    add(ids.v2, Rational(10), Rational(1));
    const auto two = copyClips(project, ids.sequence, {clipsOf(ids.v1)[0].id, clipsOf(ids.v2)[0].id});
    EXPECT_THROW((void)makePaste(project, ids.sequence, two, Rational(20), {ids.v2, {}}, false), CommandError);
    EXPECT_THROW((void)makePaste(project, ids.sequence, Clipboard{}, Rational(0), {}, false), CommandError);
}

} // namespace
