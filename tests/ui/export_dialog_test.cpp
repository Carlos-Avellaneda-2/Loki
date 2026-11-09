#include "app/export_dialog.h"
#include "app/main_window.h"
#include "commands/project_commands.h"
#include "media/media_probe.h"
#include "media_assets.h"
#include "test_temp_dir.h"
#include "ui/program_monitor_panel.h"
#include "ui/project_controller.h"

#include <QSignalSpy>

#include <gtest/gtest.h>

namespace {

using namespace loki;
using loki::test::MediaAssets;

TEST(ExportDialogTest, ExportsTheActiveSequenceInOutRangeOnAWorkerThread) {
    playback::PlaybackOptions options;
    options.allowHardware = false;
    options.deviceFactory = [](audio::RenderCallback cb) { return std::make_unique<audio::ManualAudioDevice>(std::move(cb)); };
    app::MainWindow window(nullptr, options);
    auto& controller = *window.projectController();
    model::MediaItem item;
    item.name = "clip";
    item.path = MediaAssets::get().mpeg4WithAudio;
    item.info = media::probeMedia(item.path);
    auto add = std::make_unique<commands::AddMediaCommand>(item);
    const auto id = add->mediaId();
    controller.execute(std::move(add));
    window.sourceEdit(id, false);
    controller.execute(commands::setSequenceMarkOut(controller.project(), window.activeSequence(), Rational(1, 2)));

    loki::test::TempDir dir;
    const auto file = dir.path() / "dialog.mov";
    app::ExportDialog dialog(controller, window.activeSequence());
    dialog.setForceCpu(true); // CI has no GPU
    dialog.setPreset("prores-hq");
    dialog.setOutputFile(QString::fromStdWString(file.wstring()));
    dialog.setUseInOut(true);
    QSignalSpy finished(&dialog, &app::ExportDialog::exportFinished);
    ASSERT_TRUE(dialog.startExport());
    EXPECT_TRUE(dialog.isRunning());
    EXPECT_FALSE(dialog.startExport()); // one export at a time
    dialog.waitForFinished();
    ASSERT_EQ(finished.count(), 1);
    EXPECT_TRUE(finished.front()[0].toBool()) << finished.front()[1].toString().toStdString();
    EXPECT_EQ(dialog.lastResult().status, exporter::ExportStatus::Finished);
    const auto info = media::probeMedia(file);
    EXPECT_NEAR(info.duration.toDouble(), 0.5, 0.06);
    EXPECT_EQ(info.videoCodec, "prores");
}

} // namespace
