// Phase 14 commands: curves (D33), masks (D34) and analysis samples (D36).
#include "commands/effect_commands.h"
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

class EffectsPhase14Test : public ::testing::Test {
protected:
    test::BasicProject ids;
    Project project = ids.build();
    UndoStack stack{project};
    ClipId clip;

    void SetUp() override {
        auto cmd = std::make_unique<AddClipCommand>(ids.v1, makeClip(ids.media, Rational(0), Rational(4)));
        clip = cmd->clipId();
        stack.push(std::move(cmd));
    }
    const Clip& c() { return *project.findClip(clip).clip; }
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

TEST_F(EffectsPhase14Test, CurvesAreStoredAndIdentityRemovesThem) {
    pushVerified(setEffectCurve(project, clip, kLumetriEffect, kCurveMaster, {{0, 0}, {0.5, 0.7}, {1, 1}}));
    const auto* lumetri = findEffectByType(c(), kLumetriEffect);
    ASSERT_NE(lumetri, nullptr);
    EXPECT_EQ(lumetri->curves.at("master").size(), 3u);
    pushVerified(setEffectCurve(project, clip, kLumetriEffect, kCurveMaster, identityCurve()));
    EXPECT_TRUE(findEffectByType(c(), kLumetriEffect)->curves.empty());
    EXPECT_THROW((void)setEffectCurve(project, clip, kLumetriEffect, kCurveRed, {{0.5, 0.5}}), CommandError);
    EXPECT_THROW((void)setEffectCurve(project, clip, kColorEffect, kCurveRed, identityCurve()), CommandError);
}

TEST_F(EffectsPhase14Test, MasksAreAddedEditedAndRemoved) {
    Mask mask;
    mask.path = ellipsePath(960, 540, 200, 150);
    MaskId added;
    pushVerified(addEffectMask(project, clip, kOpacityEffect, mask, &added));
    const auto* opacity = findEffectByType(c(), kOpacityEffect);
    ASSERT_NE(opacity, nullptr);
    ASSERT_EQ(opacity->masks.size(), 1u);
    EXPECT_EQ(opacity->masks[0].id, added);
    EXPECT_EQ(opacity->masks[0].name, "Mask (1)");
    auto edited = opacity->masks[0];
    edited.feather.value = 60.0;
    edited.inverted = true;
    edited.keyframes = {{Rational(1), rectanglePath(0, 0, 10, 10)}, {Rational(2), rectanglePath(5, 5, 10, 10)}};
    pushVerified(updateEffectMask(project, clip, kOpacityEffect, edited));
    EXPECT_EQ(findEffectByType(c(), kOpacityEffect)->masks[0].feather.value, 60.0);
    pushVerified(removeEffectMask(project, clip, kOpacityEffect, added));
    EXPECT_TRUE(findEffectByType(c(), kOpacityEffect)->masks.empty());
    EXPECT_THROW((void)addEffectMask(project, clip, kUltraKeyEffect, mask), CommandError);
    Mask broken;
    broken.path = {{0, 0}, {1, 1}};
    EXPECT_THROW((void)addEffectMask(project, clip, kColorEffect, broken), CommandError);
}

TEST_F(EffectsPhase14Test, AnalysisSamplesAreStored) {
    pushVerified(setEffectSamples(project, clip, kStabilizerEffect, {1, 30, 1, 0, 1, 1920, 1080, 0}));
    EXPECT_EQ(findEffectByType(c(), kStabilizerEffect)->samples.size(), 8u);
    pushVerified(applyEffect(project, clip, kGaussianBlurEffect));
    EXPECT_NE(findEffectByType(c(), kGaussianBlurEffect), nullptr);
}

} // namespace
