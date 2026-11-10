// Phase 15 UI: export queue (D41), render previews (D42), captions panel and burn-in (D40),
// multicam source and camera cuts (D43), track automation display (D39).
#include "app/main_window.h"
#include "commands/caption_commands.h"
#include "commands/project_commands.h"
#include "export/export_job.h"
#include "media/media_probe.h"
#include "playback/render_previews.h"
#include "ui/captions_panel.h"
#include "ui/export_queue.h"
#include "ui/program_monitor_panel.h"
#include "ui/project_controller.h"
#include "ui/timeline/timeline_panel.h"
#include "ui/timeline/timeline_view.h"

#include "media_assets.h"
#include "test_temp_dir.h"

#include <QApplication>
#include <QElapsedTimer>

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

class DeliveryTest : public ::testing::Test {
protected:
    std::unique_ptr<app::MainWindow> window;
    model::MediaId media;
    test::TempDir dir;

    void SetUp() override {
        playback::PlaybackOptions options;
        options.allowHardware = false;
        options.deviceFactory = [](audio::RenderCallback cb) { return std::make_unique<audio::ManualAudioDevice>(std::move(cb)); };
        window = std::make_unique<app::MainWindow>(nullptr, options);
        window->exportQueue()->setForceCpu(true);
        model::MediaItem item;
        item.name = "clip";
        item.path = MediaAssets::get().mpeg4WithAudio;
        item.info = media::probeMedia(item.path);
        auto cmd = std::make_unique<commands::AddMediaCommand>(item);
        media = cmd->mediaId();
        controller().execute(std::move(cmd));
        window->sourceEdit(media, false); // 2 s at 0 on V1/A1
    }
    ui::ProjectController& controller() { return *window->projectController(); }
    const model::Sequence& active() { return *controller().project().findSequence(window->activeSequence()); }
};

TEST_F(DeliveryTest, ExportQueueRunsJobsOneAfterTheOther) {
    auto& queue = *window->exportQueue();
    exporter::ExportRequest request;
    request.snapshot = playback::makeSnapshot(controller().project(), window->activeSequence(), 0);
    request.in = Rational(0);
    request.out = Rational(1, 2);
    request.preset = *exporter::findExportPreset("wav");
    request.file = dir.path() / "a.wav";
    auto second = request;
    second.file = dir.path() / "b.wav";
    queue.enqueue("a.wav", request);
    queue.enqueue("b.wav", second);
    EXPECT_TRUE(queue.busy());
    queue.waitForIdle();
    ASSERT_EQ(queue.entries().size(), 2u);
    EXPECT_EQ(queue.entries()[0].state, ui::ExportQueue::State::Finished);
    EXPECT_EQ(queue.entries()[1].state, ui::ExportQueue::State::Finished);
    EXPECT_TRUE(std::filesystem::exists(dir.path() / "b.wav"));
    queue.clearFinished();
    EXPECT_TRUE(queue.entries().empty());
}

TEST_F(DeliveryTest, RenderInToOutRendersTheSegmentsAndPlaybackUsesThem) {
    // Small frames keep the (CPU) render quick.
    auto settings = active().settings;
    settings.width = 320;
    settings.height = 240;
    controller().execute(std::make_unique<commands::SetSequenceSettingsCommand>(window->activeSequence(), settings));
    window->deleteRenderFiles();
    const int queued = window->renderPreviews();
    ASSERT_GE(queued, 1); // 2-second segments
    window->exportQueue()->waitForIdle();
    processEventsFor(50);
    auto snapshot = playback::makeSnapshot(controller().project(), window->activeSequence(), 0);
    const auto states = playback::previewStates(*snapshot, std::filesystem::path(window->previewCacheDir().toStdWString()));
    ASSERT_EQ(states.size(), static_cast<std::size_t>(queued));
    for (const auto s : states) {
        EXPECT_EQ(s, playback::PreviewState::Rendered);
    }
    EXPECT_EQ(window->renderPreviews(), 0); // nothing left
    window->deleteRenderFiles();
}

TEST_F(DeliveryTest, CaptionsPanelAddsImportsExportsAndBurnsIn) {
    auto* panel = window->captionsPanel();
    window->programMonitor()->engine().seek(Rational(1, 2));
    processEventsFor(30);
    panel->addAtPlayhead();
    ASSERT_EQ(active().captions.size(), 1u);
    panel->applyText(QStringLiteral("Hola mundo"));
    EXPECT_EQ(active().captions[0].text, "Hola mundo");
    const auto srt = dir.path() / "subs.srt";
    ASSERT_TRUE(panel->exportSrt(QString::fromStdWString(srt.wstring())));
    panel->deleteSelected();
    EXPECT_TRUE(active().captions.empty());
    ASSERT_TRUE(panel->importSrt(QString::fromStdWString(srt.wstring())));
    ASSERT_EQ(active().captions.size(), 1u);
    EXPECT_EQ(active().captions[0].text, "Hola mundo");
    EXPECT_EQ(panel->captionCount(), 1);
    // The picture gets a caption layer while the style says "visible".
    auto snapshot = playback::makeSnapshot(controller().project(), window->activeSequence(), 0);
    snapshot->showCaptions = true;
    playback::SequenceVideoSource src(snapshot, false, 0, 0);
    const auto f = src.frameAt(Rational(1));
    ASSERT_TRUE(f.has_value() && f->composite);
    ASSERT_EQ(f->composite->layers.size(), 2u);
    EXPECT_EQ(f->composite->layers.back().frame->format, media::PixelFormat::Rgba);
}

TEST_F(DeliveryTest, MulticamSourceAndCameraCutsFromTheMonitor) {
    ASSERT_TRUE(window->createMulticamSource({media, media}));
    model::MediaId multicam;
    for (const auto& m : controller().project().media()) {
        if (m.multicam) {
            multicam = m.id;
        }
    }
    ASSERT_TRUE(multicam);
    // Put the multicam clip on V2 and cut to camera 2 at 1 s.
    auto clip = model::Clip{};
    clip.id = model::ClipId::generate();
    clip.media = multicam;
    clip.duration = Rational(2);
    controller().execute(std::make_unique<commands::AddClipCommand>(active().videoTracks[1].id, clip));
    window->programMonitor()->engine().seek(Rational(1));
    processEventsFor(30);
    emit window->programMonitor()->cameraRequested(1);
    const auto& clips = active().videoTracks[1].clips;
    ASSERT_EQ(clips.size(), 2u);
    EXPECT_EQ(clips[1].angle, 1);
    window->programMonitor()->setMulticamView(true);
    EXPECT_TRUE(window->programMonitor()->multicamView());
}

TEST_F(DeliveryTest, TrackAutomationIsShownPerTrack) {
    auto& view = *window->timeline()->view();
    const auto a1 = active().audioTracks[0].id;
    EXPECT_FALSE(view.trackAutomation(a1).has_value());
    view.showTrackAutomation(a1, commands::AutomationKind::Volume);
    EXPECT_EQ(view.trackAutomation(a1), commands::AutomationKind::Volume);
    view.repaint(); // paints the automation line without keyframes
    controller().execute(commands::setTrackAutomationValue(controller().project(), a1, commands::AutomationKind::Volume,
                                                           Rational(1), -12.0));
    view.repaint();
    view.showTrackAutomation(a1, std::nullopt);
    EXPECT_FALSE(view.trackAutomation(a1).has_value());
}

} // namespace
