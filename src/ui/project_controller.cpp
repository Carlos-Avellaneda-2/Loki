#include "ui/project_controller.h"

#include <utility>

#include "model/project_json.h"
#include "util/atomic_file.h"

#include <QAction>
#include <QCoreApplication>
#include <QFileInfo>
#include <QKeySequence>
#include <QtConcurrent/QtConcurrentRun>

#include <spdlog/spdlog.h>

#include <exception>
#include <filesystem>

namespace loki::ui {

namespace {

std::filesystem::path toPath(const QString& path) {
    return std::filesystem::path(path.toStdWString());
}

QString fromUtf8(const std::string& text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

} // namespace

ProjectController::ProjectController(QObject* parent)
    : QObject(parent), m_project(model::makeDefaultProject()), m_undoStack(m_project) {
    qRegisterMetaType<loki::model::ChangeEvent>();
    m_project.addObserver(this);

    m_undoAction = new QAction(tr("&Undo"), this);
    m_undoAction->setShortcut(QKeySequence::Undo);
    connect(m_undoAction, &QAction::triggered, this, [this] { m_undoStack.undo(); });

    m_redoAction = new QAction(tr("&Redo"), this);
    m_redoAction->setShortcuts({QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Z), QKeySequence(Qt::CTRL | Qt::Key_Y)});
    connect(m_redoAction, &QAction::triggered, this, [this] { m_undoStack.redo(); });

    m_undoStack.setChangedCallback([this] { onUndoStackChanged(); });
    connect(&m_saveWatcher, &QFutureWatcher<QString>::finished, this, &ProjectController::onSaveDone);
    connect(&m_loadWatcher, &QFutureWatcher<LoadResult>::finished, this, &ProjectController::onLoadDone);
    onUndoStackChanged();
}

ProjectController::~ProjectController() {
    m_undoStack.setChangedCallback({});
    m_project.removeObserver(this);
    // Workers never touch m_project; waiting only keeps files consistent on shutdown.
    m_saveWatcher.waitForFinished();
    m_loadWatcher.waitForFinished();
}

bool ProjectController::execute(std::unique_ptr<commands::Command> command) {
    const auto text = command ? command->text() : std::string("(null)");
    try {
        m_undoStack.push(std::move(command));
        return true;
    } catch (const std::exception& e) {
        spdlog::warn("Command '{}' rejected: {}", text, e.what());
        emit commandFailed(fromUtf8(e.what()));
        return false;
    }
}

QString ProjectController::displayName() const {
    return m_filePath.isEmpty() ? tr("Untitled") : QFileInfo(m_filePath).completeBaseName();
}

void ProjectController::newProject() {
    replaceProject(model::makeDefaultProject(), QString());
}

bool ProjectController::openProject(const QString& path) {
    if (isBusy()) {
        return false;
    }
    m_pendingLoadPath = path;
    m_loadWatcher.setFuture(QtConcurrent::run([file = toPath(path)]() -> LoadResult {
        try {
            return {std::make_shared<model::Project>(model::loadProjectFile(file)), {}};
        } catch (const std::exception& e) {
            return {nullptr, fromUtf8(e.what())};
        }
    }));
    return true;
}

bool ProjectController::openRecovered(const QString& recoveryFile, const QString& originalPath) {
    if (!openProject(recoveryFile)) {
        return false;
    }
    m_recoveredPath = originalPath;
    return true;
}

bool ProjectController::saveProject() {
    return !m_filePath.isEmpty() && startSave(m_filePath);
}

bool ProjectController::saveProjectAs(const QString& path) {
    return !path.isEmpty() && startSave(path);
}

bool ProjectController::startSave(const QString& path) {
    if (isBusy()) {
        return false;
    }
    std::string content;
    try {
        // Serialize on the UI thread (fast, consistent snapshot); write on a worker.
        content = model::serializeProject(m_project, toPath(path));
    } catch (const std::exception& e) {
        emit saveFinished(false, fromUtf8(e.what()));
        return false;
    }
    m_pendingSavePath = path;
    m_pendingSavePoint = m_undoStack.currentPoint();
    m_saveWatcher.setFuture(QtConcurrent::run([file = toPath(path), content = std::move(content)]() -> QString {
        try {
            util::writeFileAtomic(file, content);
            return {};
        } catch (const std::exception& e) {
            return fromUtf8(e.what());
        }
    }));
    return true;
}

void ProjectController::onSaveDone() {
    const QString error = m_saveWatcher.result();
    if (!error.isEmpty()) {
        spdlog::error("Saving {} failed: {}", m_pendingSavePath.toStdString(), error.toStdString());
        emit saveFinished(false, error);
        return;
    }
    setFilePath(m_pendingSavePath);
    m_forceModified = false;
    if (!m_undoStack.markCleanAt(m_pendingSavePoint)) {
        spdlog::info("Project changed while saving; it stays modified");
    }
    spdlog::info("Project saved to {}", m_pendingSavePath.toStdString());
    emit saveFinished(true, QString());
}

void ProjectController::onLoadDone() {
    LoadResult result = m_loadWatcher.result();
    // Taken before any early return: a failed recovery must not turn the next open into one.
    const auto recovered = std::exchange(m_recoveredPath, std::nullopt);
    if (!result.project) {
        spdlog::error("Opening {} failed: {}", m_pendingLoadPath.toStdString(), result.error.toStdString());
        emit openFinished(false, result.error);
        return;
    }
    replaceProject(std::move(*result.project), recovered ? *recovered : m_pendingLoadPath);
    if (recovered) {
        m_forceModified = true;
        onUndoStackChanged(); // emits modifiedChanged(true)
    }
    spdlog::info("Project opened from {}", m_pendingLoadPath.toStdString());
    emit openFinished(true, QString());
}

void ProjectController::replaceProject(model::Project project, const QString& path) {
    // Whole-document replacement (new/open) is not an edit: it resets the history.
    // Move-assignment keeps this Project object (and its observers); only the data changes.
    m_project = std::move(project);
    m_forceModified = false;
    m_undoStack.clear();
    setFilePath(path);
    emit projectReset();
}

void ProjectController::setFilePath(const QString& path) {
    if (path != m_filePath) {
        m_filePath = path;
        emit filePathChanged(path);
    }
}

void ProjectController::waitForPendingIo() {
    if (m_saveWatcher.isRunning()) {
        m_saveWatcher.waitForFinished();
    }
    if (m_loadWatcher.isRunning()) {
        m_loadWatcher.waitForFinished();
    }
    // Deliver the queued finished() signals so results are applied before returning.
    QCoreApplication::sendPostedEvents(&m_saveWatcher);
    QCoreApplication::sendPostedEvents(&m_loadWatcher);
}

void ProjectController::onProjectChanged(const model::ChangeEvent& event) {
    emit projectChanged(event);
}

void ProjectController::onUndoStackChanged() {
    m_undoAction->setEnabled(m_undoStack.canUndo());
    m_redoAction->setEnabled(m_undoStack.canRedo());
    const auto undoText = fromUtf8(m_undoStack.undoText());
    const auto redoText = fromUtf8(m_undoStack.redoText());
    m_undoAction->setText(undoText.isEmpty() ? tr("&Undo") : tr("&Undo %1").arg(undoText));
    m_redoAction->setText(redoText.isEmpty() ? tr("&Redo") : tr("&Redo %1").arg(redoText));

    const bool modified = isModified();
    if (modified != m_lastModified) {
        m_lastModified = modified;
        emit modifiedChanged(modified);
    }
}

} // namespace loki::ui
