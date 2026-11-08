#include "ui/project_panel.h"

#include "commands/project_commands.h"
#include "media_assets.h"
#include "test_temp_dir.h"
#include "ui/media_services.h"
#include "ui/project_controller.h"
#include "ui/project_item_model.h"

#include <QMimeData>
#include <QSignalSpy>
#include <QUrl>

#include <gtest/gtest.h>

namespace {

using namespace loki;
using loki::test::MediaAssets;
using loki::ui::MediaServices;
using loki::ui::ProjectController;
using loki::ui::ProjectItemModel;

QString q(const std::filesystem::path& p) {
    return QString::fromStdWString(p.wstring());
}

class ProjectPanelTest : public ::testing::Test {
protected:
    test::TempDir cacheDir;
    ProjectController controller;
    MediaServices services{controller, q(cacheDir.path())};

    void import(const QStringList& paths, model::BinId bin = {}) {
        services.importPaths(paths, bin);
        services.waitForIdle();
    }
};

TEST_F(ProjectPanelTest, ImportProbesAndAddsAsOneUndoStep) {
    const auto& a = MediaAssets::get();
    QSignalSpy finished(&services, &MediaServices::importFinished);
    import({q(a.mpeg4WithAudio), q(a.wav), q(a.png), q(a.notMedia)});
    ASSERT_EQ(finished.count(), 1);
    EXPECT_EQ(finished.at(0).at(0).toInt(), 3);
    EXPECT_EQ(finished.at(0).at(1).toStringList().size(), 1); // notes.txt
    const auto& media = controller.project().media();
    ASSERT_EQ(media.size(), 3u);
    const auto* video = &media[0];
    for (const auto& m : media) {
        if (m.name == "mpeg4_aac") {
            video = &m;
        }
    }
    EXPECT_EQ(video->name, "mpeg4_aac");
    EXPECT_EQ(video->info.width, 320);
    EXPECT_TRUE(video->path.is_absolute());

    controller.undoAction()->trigger();
    EXPECT_TRUE(controller.project().media().empty());
}

TEST_F(ProjectPanelTest, ImportSkipsDuplicatesAndFolders) {
    const auto& a = MediaAssets::get();
    import({q(a.wav)});
    QSignalSpy finished(&services, &MediaServices::importFinished);
    import({q(a.dir)}); // whole folder, wav already present, txt filtered by extension
    ASSERT_EQ(finished.count(), 1);
    EXPECT_GE(finished.at(0).at(0).toInt(), 4); // mp4 x3 + mono wav + png
    EXPECT_EQ(finished.at(0).at(1).toStringList().size(), 1); // duplicate wav
    std::size_t wavCount = 0;
    for (const auto& m : controller.project().media()) {
        wavCount += m.name == "tone" ? 1 : 0;
    }
    EXPECT_EQ(wavCount, 1u);
}

TEST_F(ProjectPanelTest, ImportIntoBin) {
    const auto& a = MediaAssets::get();
    auto add = std::make_unique<commands::AddBinCommand>(model::Bin{{}, "Audio", {}});
    const auto bin = add->binId();
    controller.execute(std::move(add));
    import({q(a.wav)}, bin);
    ASSERT_EQ(controller.project().media().size(), 1u);
    EXPECT_EQ(controller.project().media()[0].bin, bin);
}

TEST_F(ProjectPanelTest, ThumbnailsAndWaveformsAreGeneratedAndCached) {
    const auto& a = MediaAssets::get();
    import({q(a.mpeg4WithAudio)});
    const auto item = controller.project().media()[0];
    QSignalSpy thumbs(&services, &MediaServices::thumbnailReady);
    EXPECT_TRUE(services.thumbnail(item).isNull()); // not ready yet
    services.waitForIdle();
    EXPECT_EQ(thumbs.count(), 1);
    const auto image = services.thumbnail(item);
    ASSERT_FALSE(image.isNull());
    EXPECT_EQ(image.height(), 180); // 320x240 fitted into 320x180
    EXPECT_EQ(image.width(), 240);

    EXPECT_EQ(services.waveform(item), nullptr);
    services.waitForIdle();
    const auto wf = services.waveform(item);
    ASSERT_NE(wf, nullptr);
    EXPECT_EQ(wf->channels, 2);

    // A fresh service with the same cache folder loads from disk.
    MediaServices again(controller, q(cacheDir.path()));
    (void)again.thumbnail(item);
    (void)again.waveform(item);
    again.waitForIdle();
    EXPECT_FALSE(again.thumbnail(item).isNull());
    EXPECT_NE(again.waveform(item), nullptr);
    EXPECT_GT(media::MediaCache(cacheDir.path()).diskUsage(), 0u);
}

TEST_F(ProjectPanelTest, ItemModelShowsBinsAndMediaWithMetadata) {
    const auto& a = MediaAssets::get();
    auto add = std::make_unique<commands::AddBinCommand>(model::Bin{{}, "Footage", {}});
    const auto bin = add->binId();
    controller.execute(std::move(add));
    import({q(a.mpeg4WithAudio)}, bin);
    import({q(a.wav)});

    ProjectItemModel itemModel(controller, services);
    ASSERT_EQ(itemModel.rowCount(), 2); // bin first, then root media
    const auto binIndex = itemModel.index(0, 0);
    EXPECT_TRUE(itemModel.isBin(binIndex));
    EXPECT_EQ(itemModel.data(binIndex).toString(), QStringLiteral("Footage"));
    ASSERT_EQ(itemModel.rowCount(binIndex), 1);
    const auto video = itemModel.index(0, 0, binIndex);
    EXPECT_EQ(itemModel.data(video).toString(), QStringLiteral("mpeg4_aac"));
    EXPECT_EQ(itemModel.data(itemModel.index(0, ProjectItemModel::VideoInfo, binIndex)).toString(),
              QStringLiteral("320 x 240"));
    EXPECT_EQ(itemModel.data(itemModel.index(0, ProjectItemModel::FrameRate, binIndex)).toString(),
              QStringLiteral("25 fps"));
    EXPECT_EQ(itemModel.data(itemModel.index(0, ProjectItemModel::Duration, binIndex)).toString(),
              QStringLiteral("00:00:02:00"));
    EXPECT_FALSE(itemModel.data(itemModel.index(1, ProjectItemModel::AudioInfo)).toString().isEmpty());

    // Rename through the model goes through a command.
    EXPECT_TRUE(itemModel.setData(video, QStringLiteral("Interview")));
    EXPECT_EQ(controller.project().media()[0].name, "Interview");
    EXPECT_EQ(controller.undoAction()->text(), QStringLiteral("&Undo Rename"));
}

TEST_F(ProjectPanelTest, DragAndDropMovesIntoBinAndImportsUrls) {
    const auto& a = MediaAssets::get();
    auto add = std::make_unique<commands::AddBinCommand>(model::Bin{{}, "Target", {}});
    const auto bin = add->binId();
    controller.execute(std::move(add));
    import({q(a.wav)});
    ProjectItemModel itemModel(controller, services);
    const auto mediaIndex = itemModel.index(1, 0);
    std::unique_ptr<QMimeData> mime(itemModel.mimeData({mediaIndex}));
    ASSERT_EQ(ProjectItemModel::decodeMediaIds(mime.get()).size(), 1u);
    itemModel.dropMimeData(mime.get(), Qt::MoveAction, -1, 0, itemModel.indexOfBin(bin));
    EXPECT_EQ(controller.project().media()[0].bin, bin);

    QMimeData urls;
    urls.setUrls({QUrl::fromLocalFile(q(a.png))});
    EXPECT_TRUE(itemModel.dropMimeData(&urls, Qt::CopyAction, -1, 0, itemModel.indexOfBin(bin)));
    services.waitForIdle();
    EXPECT_EQ(controller.project().media().size(), 2u);
}

TEST_F(ProjectPanelTest, PanelBuildsAndSwitchesViews) {
    ui::ProjectPanel panel(controller, services);
    EXPECT_FALSE(panel.isIconView());
    panel.setIconView(true);
    EXPECT_TRUE(panel.isIconView());
    EXPECT_NE(panel.importAction(), nullptr);
}

} // namespace
