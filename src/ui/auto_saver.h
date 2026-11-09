#pragma once

#include <QDateTime>
#include <QFuture>
#include <QObject>
#include <QString>

#include <memory>
#include <vector>

class QLockFile;
class QTimer;

namespace loki::ui {

class ProjectController;

// Crash protection (D25):
//  - Recovery: a few seconds after every change the project is written to
//    <folder>/session-<id>.loki (atomic write on a worker). Each running Loki holds
//    <folder>/session-<id>.lock; the files are removed when the project is saved or Loki exits
//    normally. After a crash the lock is free, so the next start finds and offers the file.
//  - Autosave: every `intervalMinutes`, if there are changes, a timestamped copy
//    <folder>/<project>-yyyyMMdd-HHmmss.loki is written; only the newest `keep` per project stay.
class AutoSaver final : public QObject {
    Q_OBJECT

public:
    struct Settings {
        bool enabled = true;
        int intervalMinutes = 5;
        int keep = 10;
        int recoveryDelayMs = 2000;
    };
    struct Recovery {
        QString file;         // session-<id>.loki to open
        QString originalPath; // project file it belongs to (empty = untitled)
        QDateTime saved;
    };

    AutoSaver(ProjectController& controller, QString folder, Settings settings, QObject* parent = nullptr);
    ~AutoSaver() override; // normal exit: removes this session's recovery file

    AutoSaver(const AutoSaver&) = delete;
    AutoSaver& operator=(const AutoSaver&) = delete;
    AutoSaver(AutoSaver&&) = delete;
    AutoSaver& operator=(AutoSaver&&) = delete;

    void setSettings(Settings settings);
    [[nodiscard]] const Settings& settings() const noexcept { return m_settings; }
    [[nodiscard]] QString folder() const { return m_folder; }
    [[nodiscard]] QString recoveryFile() const;

    // Writes now (tests; also used before risky operations). Blocks until written.
    void writeRecoveryNow();
    void autosaveNow();
    // Autosave copies of a project (newest first).
    [[nodiscard]] QStringList autosaveFiles(const QString& projectName) const;

    // Recovery files left by sessions that did not exit normally (their lock is free or stale).
    [[nodiscard]] static std::vector<Recovery> findOrphans(const QString& folder);
    // Deletes an orphan after it was recovered.
    static void discard(const Recovery& recovery);
    // Moves an orphan that was not recovered to "<project>-recovered-yyyyMMdd-HHmmss.loki" in the
    // same folder (never pruned) and removes its session files. Returns the new path (empty on error).
    static QString keepAsAutosave(const Recovery& recovery);

    // Settings stored in QSettings ("autosave/...").
    [[nodiscard]] static Settings loadSettings();
    static void saveSettings(const Settings& settings);

signals:
    void recoveryWritten(const QString& file);
    void autosaveWritten(const QString& file);

private:
    void scheduleRecovery();
    void write(const QString& file, bool recovery);
    void removeRecovery();
    void prune(const QString& projectName) const;
    [[nodiscard]] QString projectName() const;

    ProjectController& m_controller;
    QString m_folder;
    QString m_session;
    Settings m_settings;
    std::unique_ptr<QLockFile> m_lock;
    QTimer* m_recoveryTimer = nullptr;
    QTimer* m_autosaveTimer = nullptr;
    bool m_dirtySinceAutosave = false;
    QFuture<void> m_writing;
};

} // namespace loki::ui
