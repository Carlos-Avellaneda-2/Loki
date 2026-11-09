#include "app/main_window.h"
#include "commands/edit_commands.h"
#include "commands/project_commands.h"
#include "media/media_probe.h"
#include "media_assets.h"
#include "model/effect_registry.h"
#include "playback/sources.h"
#include "ui/effect_controls_panel.h"
#include "ui/program_monitor_panel.h"
#include "ui/project_controller.h"
#include "ui/timeline/timeline_panel.h"

#include <gtest/gtest.h>

namespace {

using namespace loki;
using loki::test::MediaAssets;

class EffectControlsTest : public ::testing::Test {
protected:
    std::unique_ptr<app::MainWindow> window;
    model::MediaId media;

    void SetUp() override {
        playback::PlaybackOptions options;
        options.allowHardware = false;
        options.deviceFactory = [](audio::RenderCallback cb) {
            return std::make_unique<audio::ManualAudioDevice>(std::move(cb));
        };
        window = std::make_unique<app::MainWindow>(nullptr, options);
        model::MediaItem item;
        item.name = "clip";
        item.path = MediaAssets::get().mpeg4WithAudio;
        item.info = media::probeMedia(item.path);
        auto cmd = std::make_unique<commands::AddMediaCommand>(item);
        media = cmd->mediaId();
        controller().execute(std::move(cmd));
        window->sourceEdit(media, false); // 2 s A/V at 0 on V1/A1
    }

    ui::ProjectController& controller() { return *window->projectController(); }
    const model::Sequence& sequence() { return *controller().project().findSequence(window->activeSequence()); }
    const model::Clip& videoClip() { return sequence().videoTracks[0].clips[0]; }
    void seek(Rational t) {
        window->programMonitor()->engine().seek(t);
        ASSERT_TRUE(window->programMonitor()->engine().waitIdle());
    }
};

TEST_F(EffectControlsTest, SelectingALinkedPairShowsTheVideoClip) {
    auto* view = window->timeline()->view();
    view->setSelection({sequence().audioTracks[0].clips[0].id, videoClip().id});
    EXPECT_EQ(window->effectControls()->clip(), videoClip().id);
    view->setSelection({});
    EXPECT_FALSE(window->effectControls()->clip());
}

TEST_F(EffectControlsTest, EditsAnimateAtThePlayhead) {
    auto* panel = window->effectControls();
    panel->setClip(videoClip().id);
    seek(Rational(1, 2));
    EXPECT_EQ(panel->playheadSourceTime(), Rational(1, 2));
    panel->setAnimated(std::string(model::kOpacityEffect), "opacity", true);
    seek(Rational(3, 2));
    panel->setValue(std::string(model::kOpacityEffect), "opacity", 0.0, 0);
    const auto* opacity = model::findEffectByType(videoClip(), model::kOpacityEffect)->findParameter("opacity");
    ASSERT_EQ(opacity->keyframes.size(), 2u);
    EXPECT_EQ(opacity->keyframes[0].time, Rational(1, 2));
    EXPECT_EQ(opacity->keyframes[1].time, Rational(3, 2));
    EXPECT_DOUBLE_EQ(opacity->valueAt(Rational(1)), 50.0);

    // Keyframe navigation moves the playhead.
    panel->jumpKeyframe(std::string(model::kOpacityEffect), "opacity", false);
    ASSERT_TRUE(window->programMonitor()->engine().waitIdle());
    EXPECT_EQ(window->programMonitor()->engine().position(), Rational(1, 2));
}

TEST_F(EffectControlsTest, ProgramCompositeReflectsMotionAndOpacity) {
    auto* panel = window->effectControls();
    panel->setClip(videoClip().id);
    seek(Rational(1, 2));
    panel->setValue(std::string(model::kMotionEffect), "scale", 50.0, 0);
    panel->setValue(std::string(model::kOpacityEffect), "opacity", 25.0, 0);
    window->programMonitor()->publishSnapshot();
    auto& engine = window->programMonitor()->engine();
    ASSERT_TRUE(engine.waitIdle());
    (void)engine.currentFrame();
    const auto picture = engine.currentPicture();
    ASSERT_TRUE(picture.has_value());
    ASSERT_NE(picture->composite, nullptr);
    ASSERT_EQ(picture->composite->layers.size(), 1u);
    const auto& t = picture->composite->layers[0].transform;
    EXPECT_DOUBLE_EQ(t.scale, 50.0);
    EXPECT_DOUBLE_EQ(t.opacity, 25.0);
    EXPECT_EQ(picture->composite->width, sequence().settings.width);
}

TEST_F(EffectControlsTest, ResetRemovesTheEffect) {
    auto* panel = window->effectControls();
    panel->setClip(videoClip().id);
    panel->setValue(std::string(model::kMotionEffect), "rotation", 45.0, 0);
    ASSERT_NE(model::findEffectByType(videoClip(), model::kMotionEffect), nullptr);
    panel->resetEffect(std::string(model::kMotionEffect));
    EXPECT_EQ(model::findEffectByType(videoClip(), model::kMotionEffect), nullptr);
}

} // namespace
