#include "app/main_window.h"
#include "commands/project_commands.h"
#include "export/proxy.h"
#include "media/media_probe.h"
#include "media_assets.h"
#include "model/project_json.h"
#include "playback/sources.h"
#include "test_temp_dir.h"
#include "ui/auto_saver.h"
#include "ui/media_services.h"
#include "ui/program_monitor_panel.h"
#include "ui/project_controller.h"
#include "ui/stall_detector.h"

#include <DockManager.h>
#include <DockWidget.h>

#include <QAction>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QSettings>
#include <QSignalSpy>
#include <QThread>

#include <gtest/gtest.h>

namespace {

using namespace loki;
using loki::test::MediaAssets;

void processEventsFor(int ms) {
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(2);
    }
}

model::MediaItem videoItem() {
    model::MediaItem item;
    item.name = "clip";
    item.path = MediaAssets::get().mpeg4WithAudio;
    item.info = media::probeMedia(item.path);
    return item;
}

// --- Proxies (D24) ---------------------------------------------------------------------------

TEST(ProxyTest, MakesASmallVideoOnlyProxyAndPlaybackPrefersIt) {
    loki::test::TempDir dir;
    auto item = videoItem(); // 320x240, 25 fps, 2 s
    item.id = model::MediaId::generate();
    EXPECT_EQ(exporter::proxySize(1920, 1080), std::make_pair(1280, 720));
    EXPECT_EQ(exporter::proxySize(320, 240), std::make_pair(320, 240)); // never upscaled
    EXPECT_EQ(exporter::proxySize(1080, 1920), std::make_pair(406, 720)); // portrait keeps aspect, even
    const auto out = exporter::proxyPathFor(dir.path(), item.id);
    std::vector<double> progress;
    const auto result = exporter::makeProxy(item, out, nullptr, [&](double p) { progress.push_back(p); });
    ASSERT_TRUE(result.ok) << result.error;
    ASSERT_FALSE(progress.empty());
    EXPECT_DOUBLE_EQ(progress.back(), 1.0);
    const auto info = media::probeMedia(out);
    EXPECT_TRUE(info.hasVideo);
    EXPECT_FALSE(info.hasAudio);
    EXPECT_EQ(info.frameRate, Rational(25));
    EXPECT_NEAR(info.duration.toDouble(), 2.0, 0.1);
    // Playback picks the proxy only when enabled and present.
    EXPECT_EQ(playback::videoPathFor(item, true), item.path);
    item.proxyPath = out;
    EXPECT_EQ(playback::videoPathFor(item, true), out);
    EXPECT_EQ(playback::videoPathFor(item, false), item.path);
    // Cancel leaves nothing behind; stills never get proxies.
    std::atomic<bool> stop{true};
    const auto cancelled = exporter::makeProxy(item, dir.path() / "cancelled.mp4", &stop);
    EXPECT_TRUE(cancelled.cancelled);
    EXPECT_FALSE(std::filesystem::exists(dir.path() / "cancelled.mp4"));
    EXPECT_TRUE(dir.path().empty() || !std::filesystem::exists(dir.path() / "cancelled.part.mp4"));
    auto still = item;
    still.info.isStill = true;
    EXPECT_FALSE(exporter::canHaveProxy(still));
}

class RobustnessTest : public ::testing::Test {
protected:
    std::unique_ptr<app::MainWindow> window;

    void SetUp() override {
        QSettings().remove(QStringLiteral("workspaces"));
        playback::PlaybackOptions options;
        options.allowHardware = false;
        options.deviceFactory = [](audio::RenderCallback cb) { return std::make_unique<audio::ManualAudioDevice>(std::move(cb)); };
        window = std::make_unique<app::MainWindow>(nullptr, options);
    }
    void TearDown() override { QSettings().remove(QStringLiteral("workspaces")); }
    ui::ProjectController& controller() { return *window->projectController(); }
};

TEST_F(RobustnessTest, CreateProxyFromServicesSetsTheProxyPathUndoably) {
    auto add = std::make_unique<commands::AddMediaCommand>(videoItem());
    const auto id = add->mediaId();
    controller().execute(std::move(add));
    QSignalSpy finished(window->mediaServices(), &ui::MediaServices::proxyFinished);
    window->mediaServices()->createProxies({id});
    EXPECT_TRUE(window->mediaServices()->isMakingProxy(id));
    ASSERT_TRUE(finished.wait(90000)); // slow under ASan on a loaded 2-core machine
    EXPECT_TRUE(finished.front()[1].toBool()) << finished.front()[2].toString().toStdString();
    const auto* media = controller().project().findMedia(id);
    ASSERT_TRUE(media->proxyPath.has_value());
    EXPECT_TRUE(std::filesystem::exists(*media->proxyPath));
    // The shared toggle switches both monitors.
    window->proxyAction()->setChecked(true);
    EXPECT_TRUE(window->programMonitor()->useProxies());
    window->proxyAction()->setChecked(false);
    EXPECT_FALSE(window->programMonitor()->useProxies());
    std::filesystem::remove(*media->proxyPath);
}

// --- Autosave & recovery (D25) -------------------------------------------------------------------

TEST_F(RobustnessTest, RecoveryFileFollowsChangesAndIsRemovedOnSave) {
    loki::test::TempDir dir;
    ui::AutoSaver::Settings settings;
    settings.recoveryDelayMs = 50;
    settings.keep = 2;
    ui::AutoSaver saver(controller(), QString::fromStdWString((dir.path() / "autosave").wstring()), settings);
    QSignalSpy written(&saver, &ui::AutoSaver::recoveryWritten);
    controller().execute(std::make_unique<commands::AddMediaCommand>(videoItem()));
    ASSERT_TRUE(written.wait(5000));
    const auto recovery = saver.recoveryFile();
    ASSERT_TRUE(QFile::exists(recovery));
    const auto recovered = model::loadProjectFile(recovery.toStdWString());
    EXPECT_EQ(recovered.media().size(), controller().project().media().size());
    // The running session is not an orphan.
    EXPECT_TRUE(ui::AutoSaver::findOrphans(saver.folder()).empty());
    // Autosave versions are pruned to `keep`.
    for (int i = 0; i < 3; ++i) {
        saver.autosaveNow();
        QThread::msleep(1100); // timestamps have one-second resolution
    }
    EXPECT_EQ(saver.autosaveFiles(QStringLiteral("Untitled")).size(), 2);
    // Saving makes the project clean: the recovery copy goes away.
    const auto project = QString::fromStdWString((dir.path() / "saved.loki").wstring());
    ASSERT_TRUE(controller().saveProjectAs(project));
    controller().waitForPendingIo();
    QCoreApplication::processEvents();
    EXPECT_FALSE(QFile::exists(recovery));
}

TEST_F(RobustnessTest, OrphanedSessionIsFoundAndRecoveredAsModified) {
    loki::test::TempDir dir;
    const auto folder = QString::fromStdWString((dir.path() / "autosave").wstring());
    QDir().mkpath(folder);
    // A crashed session: recovery file + metadata, no live lock holder.
    const auto original = QString::fromStdWString((dir.path() / "work.loki").wstring());
    {
        auto project = model::makeDefaultProject();
        const auto json = model::serializeProject(project);
        QFile f(folder + QStringLiteral("/session-dead.loki"));
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write(QByteArray::fromStdString(json));
        QFile meta(folder + QStringLiteral("/session-dead.json"));
        ASSERT_TRUE(meta.open(QIODevice::WriteOnly));
        meta.write(QStringLiteral(R"({"originalPath": "%1", "saved": "2026-11-09T10:00:00"})")
                       .arg(QString(original).replace(QLatin1Char('\\'), QLatin1Char('/')))
                       .toUtf8());
    }
    const auto orphans = ui::AutoSaver::findOrphans(folder);
    ASSERT_EQ(orphans.size(), 1u);
    EXPECT_EQ(QDir::cleanPath(orphans[0].originalPath), QDir::cleanPath(original));
    ASSERT_TRUE(controller().openRecovered(orphans[0].file, orphans[0].originalPath));
    controller().waitForPendingIo();
    EXPECT_TRUE(controller().isModified());
    EXPECT_EQ(QDir::cleanPath(controller().filePath()), QDir::cleanPath(original));
    ui::AutoSaver::discard(orphans[0]);
    EXPECT_TRUE(ui::AutoSaver::findOrphans(folder).empty());
}

// --- Workspaces (D26) ---------------------------------------------------------------------------

TEST_F(RobustnessTest, WorkspacesRearrangeAndUserLayoutsPersist) {
    using app::MainWindow;
    for (const auto w : {MainWindow::Workspace::Audio, MainWindow::Workspace::Color, MainWindow::Workspace::Editing}) {
        window->applyWorkspace(w);
        for (const auto id : ui::kAllPanels) {
            EXPECT_FALSE(window->dockWidget(id)->isClosed());
            EXPECT_NE(window->dockWidget(id)->dockAreaWidget(), nullptr);
        }
    }
    window->applyWorkspace(MainWindow::Workspace::Audio);
    ASSERT_TRUE(window->saveWorkspace(QStringLiteral("Mine")));
    EXPECT_TRUE(MainWindow::savedWorkspaces().contains(QStringLiteral("Mine")));
    window->applyWorkspace(MainWindow::Workspace::Editing);
    EXPECT_TRUE(window->applySavedWorkspace(QStringLiteral("Mine")));
    window->deleteWorkspace(QStringLiteral("Mine"));
    EXPECT_FALSE(MainWindow::savedWorkspaces().contains(QStringLiteral("Mine")));
}

// --- UI stall detector ---------------------------------------------------------------------------

TEST(StallDetectorTest, ReportsBlocksLongerThanTheThreshold) {
    ui::StallDetector detector(nullptr, 5, 50);
    processEventsFor(50);
    detector.reset();
    QSignalSpy stalled(&detector, &ui::StallDetector::stalled);
    QThread::msleep(130); // block the event loop
    processEventsFor(40);
    ASSERT_GE(stalled.count(), 1);
    EXPECT_GE(detector.longestMs(), 100.0);
    detector.reset();
    processEventsFor(100);
    EXPECT_EQ(detector.stalls(), 0);
}

} // namespace
