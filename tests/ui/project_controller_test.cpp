#include "ui/project_controller.h"

#include "commands/clip_commands.h"
#include "commands/project_commands.h"
#include "model_fixtures.h"
#include "test_temp_dir.h"
#include "util/atomic_file.h"

#include <QAction>
#include <QSignalSpy>

#include <gtest/gtest.h>

namespace {

using namespace loki;
using namespace loki::model;
using namespace loki::commands;
using loki::ui::ProjectController;

QString toQString(const std::filesystem::path& p) {
    return QString::fromStdWString(p.wstring());
}

std::unique_ptr<Command> importMedia(const std::string& name) {
    return std::make_unique<AddMediaCommand>(test::makeMedia(name));
}

TEST(ProjectControllerTest, StartsWithCleanDefaultProject) {
    ProjectController controller;
    EXPECT_EQ(controller.project().sequences().size(), 1u);
    EXPECT_FALSE(controller.isModified());
    EXPECT_FALSE(controller.undoAction()->isEnabled());
    EXPECT_FALSE(controller.redoAction()->isEnabled());
    EXPECT_EQ(controller.displayName(), QStringLiteral("Untitled"));
}

TEST(ProjectControllerTest, ExecuteEmitsChangesAndUpdatesActions) {
    ProjectController controller;
    QSignalSpy changed(&controller, &ProjectController::projectChanged);
    QSignalSpy modified(&controller, &ProjectController::modifiedChanged);

    EXPECT_TRUE(controller.execute(importMedia("a")));
    EXPECT_EQ(changed.count(), 1);
    const auto event = changed.at(0).at(0).value<ChangeEvent>();
    EXPECT_EQ(event.kind, ChangeKind::MediaAdded);
    ASSERT_EQ(modified.count(), 1);
    EXPECT_TRUE(modified.at(0).at(0).toBool());
    EXPECT_TRUE(controller.undoAction()->isEnabled());
    EXPECT_TRUE(controller.undoAction()->text().contains(QStringLiteral("Import a")));

    controller.undoAction()->trigger();
    EXPECT_TRUE(controller.project().media().empty());
    EXPECT_FALSE(controller.isModified());
    EXPECT_TRUE(controller.redoAction()->isEnabled());
    controller.redoAction()->trigger();
    EXPECT_EQ(controller.project().media().size(), 1u);
}

TEST(ProjectControllerTest, RejectedCommandEmitsFailure) {
    ProjectController controller;
    QSignalSpy failed(&controller, &ProjectController::commandFailed);
    EXPECT_FALSE(controller.execute(std::make_unique<RemoveMediaCommand>(MediaId::generate())));
    ASSERT_EQ(failed.count(), 1);
    EXPECT_FALSE(failed.at(0).at(0).toString().isEmpty());
    EXPECT_FALSE(controller.isModified());
}

TEST(ProjectControllerTest, SaveAndOpenRoundTrip) {
    test::TempDir dir;
    const auto file = toQString(dir.path() / "edit.loki");
    Project saved;
    {
        ProjectController controller;
        controller.execute(importMedia("a"));
        controller.execute(importMedia("b"));
        EXPECT_FALSE(controller.saveProject()); // no path yet
        QSignalSpy done(&controller, &ProjectController::saveFinished);
        ASSERT_TRUE(controller.saveProjectAs(file));
        EXPECT_TRUE(controller.isBusy());
        EXPECT_FALSE(controller.saveProjectAs(file)); // busy
        controller.waitForPendingIo();
        ASSERT_EQ(done.count(), 1);
        EXPECT_TRUE(done.at(0).at(0).toBool());
        EXPECT_FALSE(controller.isModified());
        EXPECT_EQ(controller.filePath(), file);
        EXPECT_EQ(controller.displayName(), QStringLiteral("edit"));
        saved = controller.project();
    }

    ProjectController other;
    other.execute(importMedia("unsaved"));
    QSignalSpy reset(&other, &ProjectController::projectReset);
    QSignalSpy opened(&other, &ProjectController::openFinished);
    ASSERT_TRUE(other.openProject(file));
    other.waitForPendingIo();
    ASSERT_EQ(opened.count(), 1);
    EXPECT_TRUE(opened.at(0).at(0).toBool());
    EXPECT_EQ(reset.count(), 1);
    EXPECT_EQ(other.project(), saved);
    EXPECT_FALSE(other.isModified());
    EXPECT_FALSE(other.undoAction()->isEnabled()); // history reset

    // Observers survive the replacement.
    QSignalSpy changed(&other, &ProjectController::projectChanged);
    other.execute(importMedia("c"));
    EXPECT_EQ(changed.count(), 1);
}

TEST(ProjectControllerTest, EditsDuringSaveKeepProjectModified) {
    test::TempDir dir;
    ProjectController controller;
    controller.execute(importMedia("a"));
    ASSERT_TRUE(controller.saveProjectAs(toQString(dir.path() / "p.loki")));
    controller.execute(importMedia("b")); // edit before the save completes
    controller.waitForPendingIo();
    EXPECT_TRUE(controller.isModified());
    controller.undoAction()->trigger();
    EXPECT_FALSE(controller.isModified()); // back to the saved state
}

TEST(ProjectControllerTest, OpenFailureKeepsCurrentProject) {
    test::TempDir dir;
    const auto bad = dir.path() / "bad.loki";
    util::writeFileAtomic(bad, "{ \"format\": \"loki-project\", \"version\": 99 }");
    ProjectController controller;
    controller.execute(importMedia("keep"));
    const Project before = controller.project();
    QSignalSpy opened(&controller, &ProjectController::openFinished);
    ASSERT_TRUE(controller.openProject(toQString(bad)));
    controller.waitForPendingIo();
    ASSERT_EQ(opened.count(), 1);
    EXPECT_FALSE(opened.at(0).at(0).toBool());
    EXPECT_TRUE(opened.at(0).at(1).toString().contains(QStringLiteral("newer")));
    EXPECT_EQ(controller.project(), before);
    EXPECT_TRUE(controller.isModified());

    ASSERT_TRUE(controller.openProject(toQString(dir.path() / "missing.loki")));
    controller.waitForPendingIo();
    EXPECT_EQ(opened.count(), 2);
    EXPECT_FALSE(opened.at(1).at(0).toBool());
}

TEST(ProjectControllerTest, SaveFailureReportsError) {
    test::TempDir dir;
    ProjectController controller;
    QSignalSpy done(&controller, &ProjectController::saveFinished);
    ASSERT_TRUE(controller.saveProjectAs(toQString(dir.path() / "no_such_dir" / "p.loki")));
    controller.waitForPendingIo();
    ASSERT_EQ(done.count(), 1);
    EXPECT_FALSE(done.at(0).at(0).toBool());
    EXPECT_TRUE(controller.filePath().isEmpty());
}

TEST(ProjectControllerTest, NewProjectResetsEverything) {
    ProjectController controller;
    controller.execute(importMedia("a"));
    QSignalSpy reset(&controller, &ProjectController::projectReset);
    controller.newProject();
    EXPECT_EQ(reset.count(), 1);
    EXPECT_TRUE(controller.project().media().empty());
    EXPECT_FALSE(controller.isModified());
    EXPECT_FALSE(controller.undoAction()->isEnabled());
}

} // namespace
