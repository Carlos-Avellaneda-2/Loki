// Phase 14 UI: Effect Controls curves and masks, the mask overlay (pen and drag) and the stabilizer
// analysis job.
#include "app/main_window.h"
#include "commands/effect_commands.h"
#include "commands/project_commands.h"
#include "media/media_probe.h"
#include "model/effect_registry.h"
#include "ui/curve_editor.h"
#include "ui/effect_controls_panel.h"
#include "ui/mask_overlay.h"
#include "ui/program_monitor_panel.h"
#include "ui/project_controller.h"
#include "ui/timeline/timeline_panel.h"
#include "ui/timeline/timeline_view.h"

#include "media_assets.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QSignalSpy>

#include <gtest/gtest.h>

namespace {

using namespace loki;
using loki::test::MediaAssets;

void processEventsFor(int ms) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) {
        QApplication::processEvents(QEventLoop::AllEvents, 10);
    }
}

class ColorEffectsTest : public ::testing::Test {
protected:
    std::unique_ptr<app::MainWindow> window;
    model::ClipId clip;

    void SetUp() override {
        playback::PlaybackOptions options;
        options.allowHardware = false;
        options.deviceFactory = [](audio::RenderCallback cb) { return std::make_unique<audio::ManualAudioDevice>(std::move(cb)); };
        window = std::make_unique<app::MainWindow>(nullptr, options);
        model::MediaItem item;
        item.name = "clip";
        item.path = MediaAssets::get().mpeg4WithAudio;
        item.info = media::probeMedia(item.path);
        auto cmd = std::make_unique<commands::AddMediaCommand>(item);
        const auto media = cmd->mediaId();
        controller().execute(std::move(cmd));
        window->sourceEdit(media, false); // 2 s at 0 on V1/A1
        clip = active().videoTracks[0].clips.at(0).id;
        window->effectControls()->setClip(clip);
    }
    ui::ProjectController& controller() { return *window->projectController(); }
    const model::Sequence& active() { return *controller().project().findSequence(window->activeSequence()); }
    const model::Clip& c() { return *controller().project().findClip(clip).clip; }
};

TEST_F(ColorEffectsTest, LumetriCurvesFromTheEditorAreOneUndoStepPerDrag) {
    ASSERT_TRUE(controller().execute(commands::applyEffect(controller().project(), clip, model::kLumetriEffect)));
    processEventsFor(20);
    auto* panel = window->effectControls();
    auto* editor = panel->findChild<ui::CurveEditor*>();
    ASSERT_NE(editor, nullptr);
    panel->setCurve(std::string(model::kLumetriEffect), "master", {{0, 0}, {0.5, 0.6}, {1, 1}}, false);
    panel->setCurve(std::string(model::kLumetriEffect), "master", {{0, 0}, {0.5, 0.7}, {1, 1}}, true);
    EXPECT_EQ(model::findEffectByType(c(), model::kLumetriEffect)->curves.at("master")[1].y, 0.7);
    controller().undoAction()->trigger(); // the whole drag
    EXPECT_TRUE(model::findEffectByType(c(), model::kLumetriEffect)->curves.empty());
}

TEST_F(ColorEffectsTest, MasksFromThePanelAndThePen) {
    auto* panel = window->effectControls();
    QSignalSpy selected(panel, &ui::EffectControlsPanel::maskSelected);
    panel->addShapeMask(std::string(model::kOpacityEffect), true);
    ASSERT_EQ(selected.count(), 1);
    const auto* opacity = model::findEffectByType(c(), model::kOpacityEffect);
    ASSERT_NE(opacity, nullptr);
    ASSERT_EQ(opacity->masks.size(), 1u);
    auto edited = opacity->masks[0];
    edited.feather.value = 80;
    panel->editMask(std::string(model::kOpacityEffect), edited);
    EXPECT_EQ(model::findEffectByType(c(), model::kOpacityEffect)->masks[0].feather.value, 80.0);

    // Pen on the overlay: three points and a click on the first one close a new mask.
    auto* overlay = window->maskOverlay();
    overlay->startPen(clip, std::string(model::kColorEffect));
    ASSERT_TRUE(controller().execute(commands::applyEffect(controller().project(), clip, model::kColorEffect)));
    const auto press = [&](double x, double y) {
        overlay->mouse(QEvent::MouseButtonPress, Qt::LeftButton, QPointF(x, y), 1.0);
        overlay->mouse(QEvent::MouseButtonRelease, Qt::LeftButton, QPointF(x, y), 1.0);
    };
    press(800, 300);
    press(1100, 300);
    press(1000, 700);
    EXPECT_TRUE(overlay->penActive());
    press(801, 301); // the first point
    EXPECT_FALSE(overlay->penActive());
    const auto colorMasks = [&] { return model::findEffectByType(c(), model::kColorEffect)->masks; };
    ASSERT_EQ(colorMasks().size(), 1u);
    EXPECT_EQ(colorMasks()[0].path.size(), 3u);
    const auto maskId = colorMasks()[0].id;

    // Dragging inside the selected mask moves it (one undo step).
    const auto before = colorMasks()[0].path[0];
    overlay->select(clip, std::string(model::kColorEffect), maskId);
    ASSERT_TRUE(overlay->mouse(QEvent::MouseButtonPress, Qt::LeftButton, QPointF(950, 450), 1.0));
    overlay->mouse(QEvent::MouseMove, Qt::NoButton, QPointF(970, 460), 1.0);
    overlay->mouse(QEvent::MouseMove, Qt::NoButton, QPointF(990, 470), 1.0);
    overlay->mouse(QEvent::MouseButtonRelease, Qt::LeftButton, QPointF(990, 470), 1.0);
    const auto after = model::findEffectByType(c(), model::kColorEffect)->masks[0].path[0];
    EXPECT_NEAR(after.x - before.x, 40.0, 1.0); // Motion at 100 %: canvas pixels = media pixels
    EXPECT_NEAR(after.y - before.y, 20.0, 1.0);
    controller().undoAction()->trigger();
    EXPECT_NEAR(model::findEffectByType(c(), model::kColorEffect)->masks[0].path[0].x, before.x, 1e-9);
    panel->deleteMask(std::string(model::kColorEffect), maskId);
    EXPECT_TRUE(model::findEffectByType(c(), model::kColorEffect)->masks.empty());
}

TEST_F(ColorEffectsTest, StabilizerAnalysisRunsInTheBackgroundAndStoresSamples) {
    ASSERT_TRUE(controller().execute(commands::applyEffect(controller().project(), clip, model::kStabilizerEffect)));
    processEventsFor(20);
    auto* panel = window->effectControls();
    QSignalSpy finished(panel, &ui::EffectControlsPanel::jobFinished);
    panel->analyzeStabilizer(std::string(model::kStabilizerEffect));
    EXPECT_TRUE(panel->jobRunning());
    for (int i = 0; i < 300 && finished.isEmpty(); ++i) {
        processEventsFor(50);
    }
    ASSERT_EQ(finished.count(), 1);
    EXPECT_TRUE(finished.at(0).at(0).toBool());
    EXPECT_FALSE(model::findEffectByType(c(), model::kStabilizerEffect)->samples.empty());
}

} // namespace
