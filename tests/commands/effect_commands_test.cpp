#include "commands/effect_commands.h"
#include "commands/undo_stack.h"
#include "model/effect_registry.h"
#include "render/composite.h"

#include "model_fixtures.h"

#include <gtest/gtest.h>

#include <cmath>

namespace {

using namespace loki;
using namespace loki::model;
using namespace loki::commands;
using loki::test::dump;
using loki::test::makeClip;

class EffectCommandsTest : public ::testing::Test {
protected:
    test::BasicProject ids; // media 1920x1080; sequence 1920x1080 30 fps
    Project project = ids.build();
    UndoStack stack{project};
    ClipId clip;

    void SetUp() override {
        auto cmd = std::make_unique<AddClipCommand>(ids.v1, makeClip(ids.media, Rational(10), Rational(10), Rational(5)));
        clip = cmd->clipId();
        stack.push(std::move(cmd));
    }

    const Clip& c() { return *project.findClip(clip).clip; }
    double value(std::string_view type, std::string_view key, Rational sourceTime) {
        const auto ctx = clipContext(project, clip);
        return effectiveValue(c(), type, key, sourceTime, ctx.media, ctx.settings);
    }
    void pushVerified(std::unique_ptr<Command> command) {
        ASSERT_NE(command, nullptr);
        const auto before = dump(project);
        stack.push(std::move(command));
        const auto after = dump(project);
        stack.undo();
        ASSERT_EQ(dump(project), before);
        stack.redo();
        ASSERT_EQ(dump(project), after);
    }
};

TEST_F(EffectCommandsTest, DefaultsWithoutStoredEffects) {
    EXPECT_TRUE(c().effects.empty());
    EXPECT_DOUBLE_EQ(value(kMotionEffect, "positionX", Rational(5)), 960.0);
    EXPECT_DOUBLE_EQ(value(kMotionEffect, "positionY", Rational(5)), 540.0);
    EXPECT_DOUBLE_EQ(value(kMotionEffect, "anchorX", Rational(5)), 960.0); // media center
    EXPECT_DOUBLE_EQ(value(kMotionEffect, "scale", Rational(5)), 100.0);
    EXPECT_DOUBLE_EQ(value(kOpacityEffect, "opacity", Rational(5)), 100.0);
    const auto ctx = clipContext(project, clip);
    const auto t = resolveTransform(c(), Rational(5), ctx.media, ctx.settings);
    EXPECT_EQ(t.blend, BlendMode::Normal);
}

TEST_F(EffectCommandsTest, FirstChangeCreatesFixedEffectWithDefaults) {
    pushVerified(setEffectParameter(project, clip, kMotionEffect, "scale", 50.0, Rational(5)));
    const auto* motion = findEffectByType(c(), kMotionEffect);
    ASSERT_NE(motion, nullptr);
    EXPECT_EQ(motion->parameters.size(), 6u);
    EXPECT_DOUBLE_EQ(motion->findParameter("scale")->value, 50.0);
    EXPECT_DOUBLE_EQ(motion->findParameter("positionX")->value, 960.0);
    EXPECT_EQ(stack.undoText(), "Change Scale");
}

TEST_F(EffectCommandsTest, ValuesAreClampedToTheParameterRange) {
    stack.push(setEffectParameter(project, clip, kOpacityEffect, "opacity", 250.0, Rational(5)));
    EXPECT_DOUBLE_EQ(value(kOpacityEffect, "opacity", Rational(5)), 100.0);
    stack.push(setEffectParameter(project, clip, kOpacityEffect, "blendMode", 2, Rational(5)));
    const auto ctx = clipContext(project, clip);
    EXPECT_EQ(resolveTransform(c(), Rational(5), ctx.media, ctx.settings).blend, BlendMode::Screen);
}

TEST_F(EffectCommandsTest, AnimateToggleAndInterpolate) {
    pushVerified(setParameterAnimated(project, clip, kOpacityEffect, "opacity", true, Rational(5)));
    // Animated: value edits create keyframes at the edit time.
    pushVerified(setEffectParameter(project, clip, kOpacityEffect, "opacity", 0.0, Rational(7)));
    EXPECT_DOUBLE_EQ(value(kOpacityEffect, "opacity", Rational(5)), 100.0);
    EXPECT_DOUBLE_EQ(value(kOpacityEffect, "opacity", Rational(6)), 50.0);
    EXPECT_DOUBLE_EQ(value(kOpacityEffect, "opacity", Rational(7)), 0.0);

    pushVerified(setKeyframeInterpolation(project, clip, kOpacityEffect, "opacity", Rational(5), Interpolation::Hold));
    EXPECT_DOUBLE_EQ(value(kOpacityEffect, "opacity", Rational(6)), 100.0);

    pushVerified(moveKeyframe(project, clip, kOpacityEffect, "opacity", Rational(7), Rational(9)));
    const auto& keys = findEffectByType(c(), kOpacityEffect)->findParameter("opacity")->keyframes;
    ASSERT_EQ(keys.size(), 2u);
    EXPECT_EQ(keys[1].time, Rational(9));
    EXPECT_THROW((void)moveKeyframe(project, clip, kOpacityEffect, "opacity", Rational(9), Rational(5))->redo(project),
                 CommandError);

    // Toggle removes an existing keyframe, adds a new one elsewhere.
    pushVerified(toggleKeyframe(project, clip, kOpacityEffect, "opacity", Rational(9)));
    EXPECT_EQ(findEffectByType(c(), kOpacityEffect)->findParameter("opacity")->keyframes.size(), 1u);
    pushVerified(toggleKeyframe(project, clip, kOpacityEffect, "opacity", Rational(8)));
    EXPECT_EQ(findEffectByType(c(), kOpacityEffect)->findParameter("opacity")->keyframes.size(), 2u);

    // Stopwatch off: keyframes go, the value at that time stays.
    pushVerified(setParameterAnimated(project, clip, kOpacityEffect, "opacity", false, Rational(5)));
    const auto* p = findEffectByType(c(), kOpacityEffect)->findParameter("opacity");
    EXPECT_TRUE(p->keyframes.empty());
    EXPECT_DOUBLE_EQ(p->value, 100.0);
}

TEST_F(EffectCommandsTest, SliderDragMergesAndResetRestoresDefaults) {
    const auto before = dump(project);
    for (int v = 90; v >= 10; v -= 20) {
        stack.push(setEffectParameter(project, clip, kMotionEffect, "rotation", v, Rational(5), GestureId{77}));
    }
    EXPECT_DOUBLE_EQ(value(kMotionEffect, "rotation", Rational(5)), 10.0);
    stack.undo();
    EXPECT_EQ(dump(project), before);
    stack.redo();
    pushVerified(resetEffect(project, clip, kMotionEffect));
    EXPECT_EQ(findEffectByType(c(), kMotionEffect), nullptr);
    EXPECT_DOUBLE_EQ(value(kMotionEffect, "rotation", Rational(5)), 0.0);
}

TEST_F(EffectCommandsTest, RejectsUnknownNames) {
    EXPECT_THROW((void)setEffectParameter(project, clip, "nope", "x", 1, Rational()), CommandError);
    EXPECT_THROW((void)setEffectParameter(project, clip, kMotionEffect, "nope", 1, Rational()), CommandError);
    EXPECT_THROW((void)setEffectParameter(project, ClipId::generate(), kMotionEffect, "scale", 1, Rational()),
                 CommandError);
}

// --- Composite math (CPU reference shared with the GPU shader) ---------------------------------

render::Layer layer(double w, double h, model::ClipTransform t) {
    render::Layer l;
    l.sourceWidth = w;
    l.sourceHeight = h;
    l.transform = t;
    return l;
}

TEST(CompositeMathTest, DefaultTransformCentersAtNativeSize) {
    model::ClipTransform t;
    t.positionX = 960;
    t.positionY = 540;
    t.anchorX = 640;
    t.anchorY = 360;
    const auto corners = render::layerCorners(layer(1280, 720, t));
    EXPECT_NEAR(corners[0][0], 320, 1e-9);
    EXPECT_NEAR(corners[0][1], 180, 1e-9);
    EXPECT_NEAR(corners[3][0], 1600, 1e-9);
    EXPECT_NEAR(corners[3][1], 900, 1e-9);
}

TEST(CompositeMathTest, ScaleAndRotationAroundAnchor) {
    model::ClipTransform t;
    t.positionX = 100;
    t.positionY = 100;
    t.anchorX = 0;
    t.anchorY = 0;
    t.scale = 200;
    t.rotation = 90; // clockwise on screen (y down)
    const auto m = render::layerToCanvas(layer(10, 10, t));
    const auto p = render::transformPoint(m, 10, 0); // right edge of the layer
    EXPECT_NEAR(p[0], 100, 1e-9);
    EXPECT_NEAR(p[1], 120, 1e-9); // rotated to point down, doubled
}

TEST(CompositeMathTest, BlendModes) {
    const render::Rgb s{0.5f, 0.25f, 1.0f};
    const render::Rgb d{0.5f, 0.5f, 0.2f};
    using model::BlendMode;
    EXPECT_FLOAT_EQ(render::blendColor(BlendMode::Multiply, s, d)[0], 0.25f);
    EXPECT_FLOAT_EQ(render::blendColor(BlendMode::Screen, s, d)[0], 0.75f);
    EXPECT_FLOAT_EQ(render::blendColor(BlendMode::Add, s, d)[2], 1.0f);
    EXPECT_FLOAT_EQ(render::blendColor(BlendMode::Darken, s, d)[1], 0.25f);
    EXPECT_FLOAT_EQ(render::blendColor(BlendMode::Lighten, s, d)[1], 0.5f);
    EXPECT_FLOAT_EQ(render::blendColor(BlendMode::Difference, s, d)[2], 0.8f);
    EXPECT_FLOAT_EQ(render::blendColor(BlendMode::Overlay, s, d)[1], 0.25f); // d >= 0.5: screen-like branch
    const auto half = render::compositePixel(BlendMode::Normal, {1, 1, 1}, {0, 0, 0}, 0.5f);
    EXPECT_FLOAT_EQ(half[0], 0.5f);
}

} // namespace
