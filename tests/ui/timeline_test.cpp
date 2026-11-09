#include "app/main_window.h"
#include "commands/edit_commands.h"
#include "commands/project_commands.h"
#include "media/media_probe.h"
#include "media_assets.h"
#include "ui/program_monitor_panel.h"
#include "ui/project_controller.h"
#include "ui/timeline/timeline_geometry.h"
#include "ui/timeline/timeline_panel.h"

#include <QAction>

#include <gtest/gtest.h>

namespace {

using namespace loki;
using namespace loki::ui::timeline;
using loki::test::MediaAssets;

// --- Geometry ------------------------------------------------------------------------------------

model::Sequence sequenceWithClips() {
    auto seq = model::makeDefaultSequence("S", {}, 2, 1);
    model::Clip a;
    a.id = model::ClipId::generate();
    a.media = model::MediaId::generate();
    a.start = Rational(1);
    a.duration = Rational(2);
    seq.videoTracks[0].clips.push_back(a);
    model::Clip b = a;
    b.id = model::ClipId::generate();
    b.start = Rational(5);
    seq.audioTracks[0].clips.push_back(b);
    return seq;
}

TEST(TimelineGeometryTest, TimeToXAndBackSnapsToFrames) {
    Metrics m;
    m.pixelsPerSecond = 100;
    m.scrollX = 50;
    EXPECT_DOUBLE_EQ(timeToX(Rational(1), m), 50.0);
    EXPECT_EQ(xToTime(50.0, m, Rational(30)), Rational(1));
    EXPECT_EQ(xToTime(51.0, m, Rational(30)), Rational(30, 30)); // within the same frame
    EXPECT_EQ(xToTime(54.0, m, Rational(30)), Rational(31, 30));
    EXPECT_EQ(xToTime(-500.0, m, Rational(30)), Rational()); // never negative
}

TEST(TimelineGeometryTest, LayoutPutsHighestVideoTrackOnTop) {
    const auto seq = sequenceWithClips();
    Metrics m;
    const auto rows = layoutTracks(seq, m);
    ASSERT_EQ(rows.size(), 3u);
    EXPECT_EQ(rows[0].id, seq.videoTracks[1].id); // V2
    EXPECT_EQ(rows[1].id, seq.videoTracks[0].id); // V1
    EXPECT_EQ(rows[2].id, seq.audioTracks[0].id); // A1 after the divider
    EXPECT_EQ(rows[0].top, m.rulerHeight);
    EXPECT_EQ(rows[2].top, m.rulerHeight + 2 * m.trackHeight + m.dividerHeight);
    EXPECT_EQ(rowAt(rows, rows[1].top + 3), &rows[1]);
    EXPECT_EQ(rowAt(rows, 0), nullptr);
    EXPECT_EQ(contentHeight(seq, m), m.rulerHeight + m.dividerHeight + 3 * m.trackHeight);
}

TEST(TimelineGeometryTest, HitTestZones) {
    const auto seq = sequenceWithClips();
    Metrics m;
    m.pixelsPerSecond = 100;
    const auto rows = layoutTracks(seq, m);
    const int y = rows[1].top + 10; // V1
    EXPECT_EQ(hitTest(seq, rows, m, 150, y).zone, HitZone::Body);
    EXPECT_EQ(hitTest(seq, rows, m, 102, y).zone, HitZone::InEdge);
    EXPECT_EQ(hitTest(seq, rows, m, 297, y).zone, HitZone::OutEdge);
    EXPECT_EQ(hitTest(seq, rows, m, 150, y).clip, seq.videoTracks[0].clips[0].id);
    const auto miss = hitTest(seq, rows, m, 400, y);
    EXPECT_FALSE(miss.clip);
    EXPECT_EQ(miss.track, seq.videoTracks[0].id);
}

TEST(TimelineGeometryTest, Snapping) {
    const auto seq = sequenceWithClips();
    Metrics m;
    m.pixelsPerSecond = 100; // 8 px tolerance = 0.08 s
    const auto candidates = snapCandidates(seq, Rational(4));
    EXPECT_EQ(candidates, (std::vector<Rational>{Rational(0), Rational(1), Rational(3), Rational(4), Rational(5),
                                                 Rational(7)}));
    EXPECT_EQ(snapTime(Rational(305, 100), candidates, m), Rational(3));
    EXPECT_FALSE(snapTime(Rational(35, 10), candidates, m).has_value());
    // Block [3.95, 5.95): its start is 0.05 s from 4 -> delta +0.05.
    EXPECT_EQ(snapBlock(Rational(395, 100), Rational(595, 100), candidates, m), Rational(5, 100));
    const auto excluded = snapCandidates(seq, Rational(), {seq.videoTracks[0].clips[0].id});
    EXPECT_EQ(std::count(excluded.begin(), excluded.end(), Rational(3)), 0);
}

TEST(TimelineGeometryTest, RulerStepAndEditPoints) {
    Metrics m;
    m.pixelsPerSecond = 2000; // zoomed in: frames
    EXPECT_EQ(rulerStep(m, Rational(30)), Rational(2, 30));
    m.pixelsPerSecond = 10;
    EXPECT_EQ(rulerStep(m, Rational(30)), Rational(10));
    const auto seq = sequenceWithClips();
    EXPECT_EQ(nextEditPoint(seq, Rational(1)), Rational(3));
    EXPECT_EQ(previousEditPoint(seq, Rational(5)), Rational(3));
    EXPECT_FALSE(nextEditPoint(seq, Rational(7)).has_value());
}

// --- Integration: Source -> Program/Timeline ------------------------------------------------------

class TimelineIntegrationTest : public ::testing::Test {
protected:
    std::unique_ptr<app::MainWindow> window;

    void SetUp() override {
        playback::PlaybackOptions options;
        options.allowHardware = false;
        options.deviceFactory = [](audio::RenderCallback cb) {
            return std::make_unique<audio::ManualAudioDevice>(std::move(cb));
        };
        window = std::make_unique<app::MainWindow>(nullptr, options);
    }

    ui::ProjectController& controller() { return *window->projectController(); }
    const model::Sequence& sequence() { return *controller().project().findSequence(window->activeSequence()); }

    model::MediaId addMedia(const std::filesystem::path& path) {
        model::MediaItem item;
        item.name = path.stem().string();
        item.path = path;
        item.info = media::probeMedia(path);
        auto cmd = std::make_unique<commands::AddMediaCommand>(item);
        const auto id = cmd->mediaId();
        controller().execute(std::move(cmd));
        return id;
    }
};

TEST_F(TimelineIntegrationTest, DefaultSequenceIsActiveEverywhere) {
    ASSERT_TRUE(window->activeSequence());
    EXPECT_EQ(window->timeline()->view()->sequenceId(), window->activeSequence());
    EXPECT_EQ(window->programMonitor()->sequenceId(), window->activeSequence());
    const auto targets = window->timeline()->view()->targets();
    EXPECT_EQ(targets.video, sequence().videoTracks[0].id);
    EXPECT_EQ(targets.audio, sequence().audioTracks[0].id);
}

TEST_F(TimelineIntegrationTest, OverwriteAndInsertFromSourceMoveThePlayhead) {
    const auto media = addMedia(MediaAssets::get().mpeg4WithAudio); // 2 s A/V
    controller().execute(commands::setMediaMarkIn(media, Rational(1, 2)));
    controller().execute(commands::setMediaMarkOut(media, Rational(3, 2)));

    window->sourceEdit(media, false);
    ASSERT_EQ(sequence().videoTracks[0].clips.size(), 1u);
    ASSERT_EQ(sequence().audioTracks[0].clips.size(), 1u);
    EXPECT_EQ(sequence().videoTracks[0].clips[0].duration, Rational(1));
    auto& engine = window->programMonitor()->engine();
    ASSERT_TRUE(engine.waitIdle());
    EXPECT_EQ(engine.position(), Rational(1)); // playhead at the end of the edit

    engine.seek(Rational(1, 2));
    ASSERT_TRUE(engine.waitIdle());
    window->sourceEdit(media, true);
    const auto& v1 = sequence().videoTracks[0].clips;
    ASSERT_EQ(v1.size(), 3u); // split + inserted + right half
    EXPECT_EQ(sequence().duration(), Rational(2));
    controller().undoAction()->trigger();
    EXPECT_EQ(sequence().videoTracks[0].clips.size(), 1u);
}

TEST_F(TimelineIntegrationTest, ProgramMonitorFollowsEdits) {
    const auto media = addMedia(MediaAssets::get().mpeg4WithAudio);
    window->sourceEdit(media, false);
    auto& engine = window->programMonitor()->engine();
    window->programMonitor()->publishSnapshot();
    ASSERT_TRUE(engine.waitIdle());
    EXPECT_EQ(engine.duration(), sequence().duration());
    controller().undoAction()->trigger();
    window->programMonitor()->publishSnapshot();
    ASSERT_TRUE(engine.waitIdle());
    EXPECT_EQ(engine.duration(), Rational());
}

TEST_F(TimelineIntegrationTest, TimelineDeleteAndRazor) {
    const auto media = addMedia(MediaAssets::get().mpeg4WithAudio);
    window->sourceEdit(media, false); // 2 s at 0
    auto* view = window->timeline()->view();
    auto& engine = window->programMonitor()->engine();
    engine.seek(Rational(1));
    ASSERT_TRUE(engine.waitIdle());
    view->razorAtPlayhead(true);
    EXPECT_EQ(sequence().videoTracks[0].clips.size(), 2u);
    EXPECT_EQ(sequence().audioTracks[0].clips.size(), 2u);
    view->setSelection({sequence().videoTracks[0].clips[0].id});
    view->deleteSelection(); // linked audio stays: only the selected clip is deleted
    EXPECT_EQ(sequence().videoTracks[0].clips.size(), 1u);
    EXPECT_EQ(sequence().audioTracks[0].clips.size(), 2u);
}

TEST_F(TimelineIntegrationTest, ZoomToFitAndToolSwitch) {
    const auto media = addMedia(MediaAssets::get().mpeg4WithAudio);
    window->sourceEdit(media, false);
    auto* view = window->timeline()->view();
    view->resize(1200, 300);
    view->zoomToFit();
    EXPECT_GT(view->metrics().pixelsPerSecond, 100.0);
    view->setTool(Tool::Razor);
    EXPECT_EQ(view->tool(), Tool::Razor);
}

} // namespace
