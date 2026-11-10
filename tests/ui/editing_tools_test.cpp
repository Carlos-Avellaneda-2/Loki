// Phase 13 UI: markers (D29), nest / open nested (D30), adjustment layers (D31), scopes (D32),
// Rate Stretch tool and the Speed dialog (D28).
#include "app/main_window.h"
#include "commands/edit_commands.h"
#include "commands/project_commands.h"
#include "ui/markers_panel.h"
#include "ui/program_monitor_panel.h"
#include "ui/project_controller.h"
#include "ui/scopes_panel.h"
#include "ui/speed_dialog.h"
#include "ui/timeline/timeline_panel.h"
#include "ui/timeline/timeline_view.h"

#include <QAction>
#include <QApplication>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QImage>
#include <QSignalSpy>

#include <gtest/gtest.h>

namespace {

using namespace loki;

void processEventsFor(int ms) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) {
        QApplication::processEvents(QEventLoop::AllEvents, 10);
    }
}

model::MediaItem fakeMedia() {
    model::MediaItem m;
    m.id = model::MediaId::generate();
    m.name = "fake";
    m.path = "C:/does/not/exist.mp4";
    m.info.hasVideo = true;
    m.info.width = 1920;
    m.info.height = 1080;
    m.info.frameRate = Rational(30);
    m.info.duration = Rational(60);
    return m;
}

class EditingToolsTest : public ::testing::Test {
protected:
    std::unique_ptr<app::MainWindow> window;

    void SetUp() override {
        playback::PlaybackOptions options;
        options.allowHardware = false;
        options.deviceFactory = [](audio::RenderCallback cb) { return std::make_unique<audio::ManualAudioDevice>(std::move(cb)); };
        window = std::make_unique<app::MainWindow>(nullptr, options);
    }
    ui::ProjectController& controller() { return *window->projectController(); }
    const model::Sequence& active() { return *controller().project().findSequence(window->activeSequence()); }
    ui::timeline::TimelineView& view() { return *window->timeline()->view(); }

    model::ClipId addClip(std::size_t track, Rational start, Rational duration) {
        if (controller().project().media().empty()) {
            controller().execute(std::make_unique<commands::AddMediaCommand>(fakeMedia()));
        }
        const auto media = controller().project().media().front().id;
        model::Clip c;
        c.id = model::ClipId::generate();
        c.media = media;
        c.start = start;
        c.duration = duration;
        controller().execute(std::make_unique<commands::AddClipCommand>(active().videoTracks[track].id, c));
        return c.id;
    }
};

TEST_F(EditingToolsTest, MarkersAreAddedNavigatedAndListed) {
    auto& t = view();
    window->programMonitor()->engine().seek(Rational(2));
    processEventsFor(30);
    t.addMarker();
    window->programMonitor()->engine().seek(Rational(5));
    processEventsFor(30);
    t.addMarker();
    ASSERT_EQ(active().markers.size(), 2u);
    EXPECT_EQ(window->markersPanel()->markerCount(), 2);
    t.goToMarker(false);
    processEventsFor(30);
    EXPECT_EQ(window->programMonitor()->engine().position(), Rational(2));
    t.goToMarker(true);
    processEventsFor(30);
    EXPECT_EQ(window->programMonitor()->engine().position(), Rational(5));
    t.clearMarkerAtPlayhead();
    EXPECT_EQ(active().markers.size(), 1u);
    EXPECT_EQ(window->markersPanel()->markerCount(), 1);
    controller().undoAction()->trigger();
    EXPECT_EQ(window->markersPanel()->markerCount(), 2);
}

TEST_F(EditingToolsTest, NestAndDoubleClickOpenTheNestedSequence) {
    const auto a = addClip(0, Rational(0), Rational(2));
    const auto main = window->activeSequence();
    model::MediaId nestMedia;
    model::SequenceId nested;
    ASSERT_TRUE(controller().execute(commands::makeNest(controller().project(), main, {a}, "Inner", &nestMedia, &nested)));
    QSignalSpy open(&view(), &ui::timeline::TimelineView::openSequenceRequested);
    emit view().openSequenceRequested(nested); // what a double-click on the nested clip emits
    EXPECT_EQ(window->activeSequence(), nested);
    EXPECT_EQ(active().videoTracks[0].clips.size(), 1u);
    // Deleting the nested sequence also removes the nested item and its clips.
    window->setActiveSequence(main);
    ASSERT_TRUE(window->deleteSequence(nested));
    EXPECT_EQ(controller().project().findSequence(nested), nullptr);
    EXPECT_EQ(controller().project().findMedia(nestMedia), nullptr);
    EXPECT_TRUE(active().videoTracks[0].clips.empty());
    controller().undoAction()->trigger();
    EXPECT_NE(controller().project().findSequence(nested), nullptr);
}

TEST_F(EditingToolsTest, AdjustmentLayerIsPlacedAtThePlayhead) {
    window->programMonitor()->engine().seek(Rational(1));
    processEventsFor(30);
    window->newAdjustmentLayer();
    const model::Clip* placed = nullptr;
    for (const auto& track : active().videoTracks) {
        for (const auto& c : track.clips) {
            placed = &c;
        }
    }
    ASSERT_NE(placed, nullptr);
    EXPECT_EQ(placed->start, Rational(1));
    EXPECT_TRUE(controller().project().findMedia(placed->media)->isAdjustment());
}

TEST_F(EditingToolsTest, RateStretchToolAndSpeedDialog) {
    auto& t = view();
    t.setTool(ui::timeline::Tool::RateStretch);
    EXPECT_EQ(t.tool(), ui::timeline::Tool::RateStretch);
    ui::SpeedDialog dialog(Rational(4), Rational(30), 100.0, false);
    EXPECT_EQ(dialog.speed(), Rational(1));
    dialog.findChild<QDoubleSpinBox*>()->setValue(50.0);
    EXPECT_EQ(dialog.speed(), Rational(1, 2));
    EXPECT_FALSE(dialog.reverse());
}

TEST_F(EditingToolsTest, ScopesAnalyzePicturesOffTheUiThread) {
    ui::ScopesPanel scopes;
    scopes.show();
    QSignalSpy analyzed(&scopes, &ui::ScopesPanel::analyzed);
    QImage image(64, 32, QImage::Format_RGBA8888);
    image.fill(QColor(255, 0, 0));
    scopes.setImage(image);
    for (int i = 0; i < 100 && analyzed.isEmpty(); ++i) {
        processEventsFor(20);
    }
    ASSERT_EQ(analyzed.count(), 1);
    ASSERT_TRUE(scopes.data());
    EXPECT_EQ(scopes.data()->samples, 64u * 32u);
    EXPECT_EQ(scopes.data()->histogram[0][255], 64u * 32u);
    for (const auto mode : {ui::ScopesPanel::Mode::Parade, ui::ScopesPanel::Mode::Vectorscope,
                            ui::ScopesPanel::Mode::Histogram, ui::ScopesPanel::Mode::Waveform}) {
        scopes.setMode(mode);
        scopes.repaint(); // paints without crashing in every mode
    }
}

} // namespace
