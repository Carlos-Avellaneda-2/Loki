#pragma once

#include "commands/clip_commands.h"
#include "commands/undo_stack.h"
#include "model/project.h"

#include <QFutureWatcher>
#include <QMetaType>
#include <QObject>
#include <QString>

#include <memory>

class QAction;

namespace loki::ui {

// Qt adapter around the pure C++ model (D1): owns the Project and its UndoStack, turns model
// change events into Qt signals, exposes undo/redo actions, and loads/saves project files on a
// worker thread so the UI thread never blocks on I/O.
class ProjectController final : public QObject, private model::ProjectObserver {
    Q_OBJECT

public:
    explicit ProjectController(QObject* parent = nullptr); // starts with the default project
    ~ProjectController() override;

    ProjectController(const ProjectController&) = delete;
    ProjectController& operator=(const ProjectController&) = delete;
    ProjectController(ProjectController&&) = delete;
    ProjectController& operator=(ProjectController&&) = delete;

    [[nodiscard]] const model::Project& project() const noexcept { return m_project; }
    [[nodiscard]] commands::UndoStack& undoStack() noexcept { return m_undoStack; }

    // Runs a command through the undo stack. Returns false (and emits commandFailed) if the
    // command was rejected; the project is then unchanged.
    bool execute(std::unique_ptr<commands::Command> command);

    // New id for one continuous gesture (drag, slider scrub): commands created with it merge
    // into a single undo step.
    [[nodiscard]] commands::GestureId newGestureId() noexcept { return m_nextGesture++; }

    [[nodiscard]] QAction* undoAction() const noexcept { return m_undoAction; }
    [[nodiscard]] QAction* redoAction() const noexcept { return m_redoAction; }

    void newProject();
    // Asynchronous; result reported by openFinished. Returns false if another load/save runs.
    bool openProject(const QString& path);
    // Asynchronous; result reported by saveFinished. Returns false if busy or no file path.
    bool saveProject();
    bool saveProjectAs(const QString& path);

    [[nodiscard]] QString filePath() const { return m_filePath; }
    [[nodiscard]] QString displayName() const;
    [[nodiscard]] bool isModified() const noexcept { return !m_undoStack.isClean(); }
    [[nodiscard]] bool isBusy() const noexcept { return m_saveWatcher.isRunning() || m_loadWatcher.isRunning(); }
    // Blocks until a pending load/save finished and its result was applied (tests, shutdown).
    void waitForPendingIo();

signals:
    void projectChanged(const loki::model::ChangeEvent& event);
    void projectReset(); // new/open replaced the whole project
    void modifiedChanged(bool modified);
    void filePathChanged(const QString& path);
    void commandFailed(const QString& message);
    void openFinished(bool ok, const QString& error);
    void saveFinished(bool ok, const QString& error);

private:
    struct LoadResult {
        std::shared_ptr<model::Project> project;
        QString error;
    };

    void onProjectChanged(const model::ChangeEvent& event) override;
    void onUndoStackChanged();
    void onSaveDone();
    void onLoadDone();
    bool startSave(const QString& path);
    void replaceProject(model::Project project, const QString& path);
    void setFilePath(const QString& path);

    model::Project m_project;
    commands::UndoStack m_undoStack;
    QAction* m_undoAction = nullptr; // owned (Qt parent)
    QAction* m_redoAction = nullptr; // owned (Qt parent)
    QString m_filePath;
    bool m_lastModified = false;
    commands::GestureId m_nextGesture = 1;

    QFutureWatcher<QString> m_saveWatcher; // result: empty = success, otherwise error message
    QString m_pendingSavePath;
    commands::UndoStack::Point m_pendingSavePoint;
    QFutureWatcher<LoadResult> m_loadWatcher;
    QString m_pendingLoadPath;
};

} // namespace loki::ui

Q_DECLARE_METATYPE(loki::model::ChangeEvent)
