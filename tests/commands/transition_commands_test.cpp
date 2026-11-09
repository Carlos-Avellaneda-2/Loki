#include "commands/edit_commands.h"
#include "commands/effect_commands.h"
#include "commands/transition_commands.h"
#include "commands/undo_stack.h"
#include "model/effect_registry.h"
#include "model/project_json.h"
#include "model/transitions.h"

#include "model_fixtures.h"

#include <gtest/gtest.h>

#include <filesystem>

namespace {

using namespace loki;
using namespace loki::model;
using namespace loki::commands;
using loki::test::dump;
using loki::test::makeClip;

class TransitionTest : public ::testing::Test {
protected:
    test::BasicProject ids; // V1, V2, A1; media 60 s
    Project project = ids.build();
    UndoStack stack{project};

    ClipId add(TrackId track, Rational start, Rational duration, Rational sourceIn = Rational(10)) {
        auto clip = makeClip(ids.media, start, duration, sourceIn);
        auto cmd = std::make_unique<AddClipCommand>(track, clip);
        const auto id = cmd->clipId();
        stack.push(std::move(cmd));
        return id;
    }
    const Track& track(TrackId id) { return *project.findTrack(id).track; }
};

TEST_F(TransitionTest, SpanGeometry) {
    Track t;
    auto a = makeClip(ids.media, Rational(0), Rational(4));
    auto b = makeClip(ids.media, Rational(4), Rational(4));
    t.clips = {a, b};
    Transition x{TransitionId::generate(), std::string(kCrossDissolve), a.id, b.id, Rational(2),
                 TransitionAlignment::Center};
    auto span = transitionSpan(t, x);
    ASSERT_TRUE(span);
    EXPECT_EQ(span->start, Rational(3));
    EXPECT_EQ(span->end, Rational(5));
    EXPECT_DOUBLE_EQ(span->progress(Rational(4)), 0.5);
    x.alignment = TransitionAlignment::EndAtCut;
    EXPECT_EQ(transitionSpan(t, x)->start, Rational(2));
    x.alignment = TransitionAlignment::StartAtCut;
    EXPECT_EQ(transitionSpan(t, x)->end, Rational(6));
    // One-sided: at the end of `from` / the start of `to`, never longer than the clip.
    Transition out{TransitionId::generate(), std::string(kDipToBlack), b.id, {}, Rational(10), {}};
    EXPECT_EQ(transitionSpan(t, out)->start, Rational(4));
    EXPECT_EQ(transitionSpan(t, out)->end, Rational(8));
    // Dormant when the clips stop touching.
    t.clips[1].start = Rational(5);
    EXPECT_FALSE(transitionSpan(t, x));
    EXPECT_FALSE(activeTransition(t, Rational(4)));
}

TEST_F(TransitionTest, ApplyAtACutIsTwoSidedAndReplaces) {
    const auto a = add(ids.v1, Rational(0), Rational(4));
    const auto b = add(ids.v1, Rational(4), Rational(4));
    stack.push(makeApplyTransition(project, a, ClipEdge::Out, kCrossDissolve));
    ASSERT_EQ(track(ids.v1).transitions.size(), 1u);
    const auto& t = track(ids.v1).transitions[0];
    EXPECT_EQ(t.from, a);
    EXPECT_EQ(t.to, b);
    EXPECT_EQ(t.duration, Rational(1));
    // Applying another one at the same cut (from the right clip's In edge) replaces it.
    stack.push(makeApplyTransition(project, b, ClipEdge::In, kWipe));
    ASSERT_EQ(track(ids.v1).transitions.size(), 1u);
    EXPECT_EQ(track(ids.v1).transitions[0].type, kWipe);
    stack.undo();
    EXPECT_EQ(track(ids.v1).transitions[0].type, kCrossDissolve);
    // One-sided at the free end of b; audio transitions are refused on video.
    stack.push(makeApplyTransition(project, b, ClipEdge::Out, kDipToBlack));
    EXPECT_EQ(track(ids.v1).transitions.size(), 2u);
    EXPECT_FALSE(track(ids.v1).transitions[1].to);
    EXPECT_THROW((void)makeApplyTransition(project, a, ClipEdge::Out, kCrossfade), CommandError);
    EXPECT_TRUE(project.validate().empty());
}

TEST_F(TransitionTest, DurationDragIsOneUndoStep) {
    const auto a = add(ids.v1, Rational(0), Rational(4));
    add(ids.v1, Rational(4), Rational(4));
    stack.push(makeApplyTransition(project, a, ClipEdge::Out, kCrossDissolve));
    const auto id = track(ids.v1).transitions[0].id;
    const auto count = stack.count();
    stack.push(setTransitionDuration(project, id, Rational(2), 9));
    stack.push(setTransitionDuration(project, id, Rational(3), 9));
    stack.push(setTransitionAlignment(project, id, TransitionAlignment::StartAtCut));
    EXPECT_EQ(stack.count(), count + 2);
    EXPECT_EQ(track(ids.v1).transitions[0].duration, Rational(3));
    stack.undo();
    stack.undo();
    EXPECT_EQ(track(ids.v1).transitions[0].duration, Rational(1));
    EXPECT_THROW((void)setTransitionDuration(project, id, Rational()), CommandError);
}

TEST_F(TransitionTest, EditsThatBreakTheCutRemoveTheTransitionAndUndoRestoresIt) {
    const auto a = add(ids.v1, Rational(0), Rational(4));
    const auto b = add(ids.v1, Rational(4), Rational(4));
    stack.push(makeApplyTransition(project, a, ClipEdge::Out, kCrossDissolve));
    const auto before = dump(project);
    stack.push(makeMoveClips(project, ids.sequence, {b}, Rational(2), 0, TrackKind::Video));
    EXPECT_TRUE(track(ids.v1).transitions.empty());
    stack.undo();
    EXPECT_EQ(dump(project), before);
    stack.push(makeDeleteClips(project, ids.sequence, {a}));
    EXPECT_TRUE(track(ids.v1).transitions.empty());
    stack.undo();
    EXPECT_EQ(dump(project), before);
}

TEST_F(TransitionTest, TitlesEffectsAndTextParametersRoundTripInVersion2) {
    MediaId titleMedia;
    stack.push(makeNewTitle(project, ids.sequence, ids.v2, Rational(1), Rational(5), "Opening", &titleMedia));
    const auto* media = project.findMedia(titleMedia);
    ASSERT_NE(media, nullptr);
    EXPECT_TRUE(media->isGenerated());
    ASSERT_EQ(track(ids.v2).clips.size(), 1u);
    const auto title = track(ids.v2).clips[0].id;
    stack.push(setEffectString(project, title, kTextEffect, "text", "Hola Loki"));
    stack.push(setEffectParameter(project, title, kTextEffect, "size", 120, Rational()));
    EXPECT_EQ(effectiveString(track(ids.v2).clips[0], kTextEffect, "text"), "Hola Loki");
    EXPECT_EQ(effectiveString(track(ids.v2).clips[0], kTextEffect, "font"), "Arial"); // default
    const auto a = add(ids.v1, Rational(0), Rational(4));
    stack.push(applyEffect(project, a, kColorEffect));
    stack.push(applyEffect(project, a, kLutEffect));
    stack.push(setEffectString(project, a, kLutEffect, "file", "C:/luts/film.cube"));
    EXPECT_THROW((void)applyEffect(project, a, kColorEffect), CommandError); // already applied
    EXPECT_THROW((void)applyEffect(project, a, kMotionEffect), CommandError);  // fixed
    stack.push(makeApplyTransition(project, a, ClipEdge::Out, kDipToBlack));
    const auto json = serializeProject(project);
    EXPECT_NE(json.find("\"version\": 2"), std::string::npos);
    EXPECT_EQ(parseProject(json), project);
    stack.undo(); // transition
    stack.undo(); // LUT file
    stack.undo(); // LUT
    stack.undo(); // color
    EXPECT_TRUE(track(ids.v1).clips[0].effects.empty());
}

TEST_F(TransitionTest, LoadsGoldenVersion2File) {
    const auto loaded = loadProjectFile(std::filesystem::path(LOKI_TEST_DATA_DIR) / "project_v2.loki");
    ASSERT_EQ(loaded.sequences().size(), 1u);
    const auto& seq = loaded.sequences()[0];
    ASSERT_EQ(seq.videoTracks[0].transitions.size(), 1u);
    const auto& t = seq.videoTracks[0].transitions[0];
    EXPECT_EQ(t.type, kCrossDissolve);
    EXPECT_EQ(t.alignment, TransitionAlignment::Center);
    EXPECT_EQ(t.duration, Rational(1));
    EXPECT_TRUE(isTransitionLive(seq.videoTracks[0], t));
    const MediaItem* title = nullptr;
    for (const auto& m : loaded.media()) {
        if (m.isGenerated()) {
            title = &m;
        }
    }
    ASSERT_NE(title, nullptr);
    EXPECT_EQ(title->generator, kTitleGenerator);
    const auto& titleClip = seq.videoTracks[1].clips[0];
    EXPECT_EQ(effectiveString(titleClip, kTextEffect, "text"), "Loki");
    // A v1 document still loads (migration v1 -> v2).
    const auto v1 = loadProjectFile(std::filesystem::path(LOKI_TEST_DATA_DIR) / "project_v1.loki");
    EXPECT_FALSE(v1.sequences().empty());
}

} // namespace
