#include "ui/source_monitor_panel.h"

#include "commands/project_commands.h"
#include "media/media_probe.h"
#include "media_assets.h"
#include "test_temp_dir.h"
#include "ui/media_services.h"
#include "ui/project_controller.h"

#include <QAction>

#include <gtest/gtest.h>

namespace {

using namespace loki;
using loki::test::MediaAssets;

class SourceMonitorTest : public ::testing::Test {
protected:
    test::TempDir cacheDir;
    ui::ProjectController controller;
    ui::MediaServices services{controller, QString::fromStdWString(cacheDir.path().wstring())};
    audio::ManualAudioDevice* device = nullptr;
    std::unique_ptr<ui::SourceMonitorPanel> panel;

    void SetUp() override {
        playback::PlaybackOptions options;
        options.allowHardware = false;
        options.deviceFactory = [this](audio::RenderCallback cb) {
            auto d = std::make_unique<audio::ManualAudioDevice>(std::move(cb));
            device = d.get();
            return d;
        };
        panel = std::make_unique<ui::SourceMonitorPanel>(controller, services, std::move(options));
    }

    model::MediaId addMedia(const std::filesystem::path& path) {
        model::MediaItem item;
        item.name = path.stem().string();
        item.path = path;
        item.info = media::probeMedia(path);
        auto cmd = std::make_unique<commands::AddMediaCommand>(item);
        const auto id = cmd->mediaId();
        controller.execute(std::move(cmd));
        return id;
    }

    void load(model::MediaId id) {
        panel->loadMedia(id);
        ASSERT_TRUE(panel->engine().waitIdle());
        panel->tick();
    }
};

TEST_F(SourceMonitorTest, LoadsMediaAndShowsTimecode) {
    const auto& a = MediaAssets::get();
    const auto id = addMedia(a.mpeg4WithAudio);
    load(id);
    EXPECT_EQ(panel->mediaId(), id);
    EXPECT_TRUE(panel->engine().isOpen());
    EXPECT_EQ(panel->timecodeText(), QStringLiteral("00:00:00:00"));
    panel->step(30);
    ASSERT_TRUE(panel->engine().waitIdle());
    panel->tick();
    EXPECT_EQ(panel->timecodeText(), QStringLiteral("00:00:01:05"));
}

TEST_F(SourceMonitorTest, MarkInOutAreUndoableModelEdits) {
    const auto& a = MediaAssets::get();
    const auto id = addMedia(a.mpeg4WithAudio);
    load(id);
    panel->step(10);
    ASSERT_TRUE(panel->engine().waitIdle());
    panel->markIn();
    panel->step(20);
    ASSERT_TRUE(panel->engine().waitIdle());
    panel->markOut();
    const auto* item = controller.project().findMedia(id);
    ASSERT_TRUE(item->markIn.has_value());
    ASSERT_TRUE(item->markOut.has_value());
    EXPECT_EQ(*item->markIn, Rational(10, 25));
    EXPECT_EQ(*item->markOut, Rational(31, 25)); // Out is exclusive: end of frame 30
    controller.undoAction()->trigger();
    EXPECT_FALSE(controller.project().findMedia(id)->markOut.has_value());
    controller.redoAction()->trigger();

    panel->goToIn();
    ASSERT_TRUE(panel->engine().waitIdle());
    EXPECT_EQ(panel->engine().position(), Rational(10, 25));
    panel->goToOut();
    ASSERT_TRUE(panel->engine().waitIdle());
    EXPECT_EQ(panel->engine().position(), Rational(30, 25));
    panel->clearInOut();
    EXPECT_FALSE(controller.project().findMedia(id)->markIn.has_value());
    EXPECT_EQ(controller.undoAction()->text(), QStringLiteral("&Undo Clear In and Out"));
}

TEST_F(SourceMonitorTest, ReopeningStartsAtMarkIn) {
    const auto& a = MediaAssets::get();
    const auto id = addMedia(a.mpeg4WithAudio);
    controller.execute(commands::setMediaMarkIn(id, Rational(1)));
    load(id);
    EXPECT_EQ(panel->engine().position(), Rational(1));
}

TEST_F(SourceMonitorTest, PlaysWithJkl) {
    const auto& a = MediaAssets::get();
    const auto id = addMedia(a.mpeg4WithAudio);
    load(id);
    panel->shuttle(playback::ShuttleKey::L);
    ASSERT_TRUE(panel->engine().waitIdle());
    EXPECT_EQ(panel->engine().rate(), 1);
    device->pump(4800);
    EXPECT_EQ(panel->engine().position(), Rational(1, 10));
    panel->shuttle(playback::ShuttleKey::K);
    ASSERT_TRUE(panel->engine().waitIdle());
    EXPECT_FALSE(panel->engine().isPlaying());
    panel->togglePlay();
    EXPECT_TRUE(panel->engine().isPlaying());
}

TEST_F(SourceMonitorTest, RemovingMediaUnloadsIt) {
    const auto& a = MediaAssets::get();
    const auto id = addMedia(a.wav);
    load(id);
    controller.execute(std::make_unique<commands::RemoveMediaCommand>(id));
    EXPECT_FALSE(panel->mediaId());
    ASSERT_TRUE(panel->engine().waitIdle());
    EXPECT_FALSE(panel->engine().isOpen());
}

} // namespace
