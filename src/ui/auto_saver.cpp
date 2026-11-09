#include "ui/auto_saver.h"

#include "model/project_json.h"
#include "ui/project_controller.h"
#include "util/atomic_file.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSettings>
#include <QtConcurrent/QtConcurrentRun>
#include <QTimer>
#include <QUuid>

#include <spdlog/spdlog.h>

#include <algorithm>

namespace loki::ui {

namespace {

std::filesystem::path toPath(const QString& s) {
    return std::filesystem::path(s.toStdWString());
}

QString metaFileFor(const QString& recoveryFile) {
    return recoveryFile.left(recoveryFile.size() - 5) + QStringLiteral(".json"); // .loki -> .json
}

} // namespace

AutoSaver::AutoSaver(ProjectController& controller, QString folder, Settings settings, QObject* parent)
    : QObject(parent), m_controller(controller), m_folder(std::move(folder)), m_settings(settings) {
    QDir().mkpath(m_folder);
    m_session = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_lock = std::make_unique<QLockFile>(QDir(m_folder).filePath(QStringLiteral("session-%1.lock").arg(m_session)));
    if (!m_lock->tryLock(0)) {
        spdlog::warn("Cannot lock the recovery session in {}", m_folder.toStdString());
    }
    m_recoveryTimer = new QTimer(this);
    m_recoveryTimer->setSingleShot(true);
    connect(m_recoveryTimer, &QTimer::timeout, this, [this] {
        if (m_controller.isModified()) {
            write(recoveryFile(), true);
        }
    });
    m_autosaveTimer = new QTimer(this);
    connect(m_autosaveTimer, &QTimer::timeout, this, [this] {
        if (m_dirtySinceAutosave && m_controller.isModified()) {
            autosaveNow();
        }
    });
    connect(&m_controller, &ProjectController::projectChanged, this, [this] {
        m_dirtySinceAutosave = true;
        scheduleRecovery();
    });
    connect(&m_controller, &ProjectController::modifiedChanged, this, [this](bool modified) {
        if (!modified) {
            removeRecovery(); // saved: the project file is the latest state again
        } else {
            scheduleRecovery();
        }
    });
    connect(&m_controller, &ProjectController::projectReset, this, [this] {
        if (!m_controller.isModified()) {
            removeRecovery();
        }
        m_dirtySinceAutosave = false;
    });
    setSettings(m_settings);
}

AutoSaver::~AutoSaver() {
    m_writing.waitForFinished();
    removeRecovery();
    m_lock->unlock();
}

void AutoSaver::setSettings(Settings settings) {
    m_settings = settings;
    if (m_settings.enabled && m_settings.intervalMinutes > 0) {
        m_autosaveTimer->start(m_settings.intervalMinutes * 60 * 1000);
    } else {
        m_autosaveTimer->stop();
    }
}

QString AutoSaver::recoveryFile() const {
    return QDir(m_folder).filePath(QStringLiteral("session-%1.loki").arg(m_session));
}

QString AutoSaver::projectName() const {
    const auto path = m_controller.filePath();
    return path.isEmpty() ? QStringLiteral("Untitled") : QFileInfo(path).completeBaseName();
}

void AutoSaver::scheduleRecovery() {
    if (m_settings.enabled) {
        m_recoveryTimer->start(m_settings.recoveryDelayMs); // restarts: written once edits pause
    }
}

void AutoSaver::writeRecoveryNow() {
    m_recoveryTimer->stop();
    write(recoveryFile(), true);
    m_writing.waitForFinished();
}

void AutoSaver::autosaveNow() {
    const auto file = QDir(m_folder).filePath(
        QStringLiteral("%1-%2.loki").arg(projectName(), QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"))));
    write(file, false);
    m_dirtySinceAutosave = false;
    m_writing.waitForFinished(); // rare (every few minutes) and needed before pruning
    prune(projectName());
}

void AutoSaver::write(const QString& file, bool recovery) {
    std::string content;
    try {
        // Serialize on the UI thread (consistent snapshot); relative media paths stay relative to
        // the real project folder.
        const auto projectFile = m_controller.filePath().isEmpty() ? toPath(file) : toPath(m_controller.filePath());
        content = model::serializeProject(m_controller.project(), projectFile);
    } catch (const std::exception& e) {
        spdlog::error("Serializing for {} failed: {}", file.toStdString(), e.what());
        return;
    }
    std::string meta;
    if (recovery) {
        meta = QJsonDocument(QJsonObject{{QStringLiteral("originalPath"), m_controller.filePath()},
                                         {QStringLiteral("saved"), QDateTime::currentDateTime().toString(Qt::ISODate)}})
                   .toJson()
                   .toStdString();
    }
    m_writing.waitForFinished(); // one write at a time (normally long finished)
    // Disk I/O on a worker: the UI thread never waits for the disk.
    m_writing = QtConcurrent::run([this, file, recovery, content = std::move(content), meta = std::move(meta)] {
        try {
            util::writeFileAtomic(toPath(file), content);
            if (recovery) {
                util::writeFileAtomic(toPath(metaFileFor(file)), meta);
            }
            QMetaObject::invokeMethod(
                this, [this, file, recovery] { recovery ? emit recoveryWritten(file) : emit autosaveWritten(file); },
                Qt::QueuedConnection);
        } catch (const std::exception& e) {
            spdlog::error("Writing {} failed: {}", file.toStdString(), e.what());
        }
    });
}

void AutoSaver::removeRecovery() {
    m_recoveryTimer->stop();
    m_writing.waitForFinished(); // a pending write must not recreate the file afterwards
    QFile::remove(recoveryFile());
    QFile::remove(metaFileFor(recoveryFile()));
}

QStringList AutoSaver::autosaveFiles(const QString& name) const {
    QDir dir(m_folder);
    auto files = dir.entryList({name + QStringLiteral("-????????-??????.loki")}, QDir::Files, QDir::Name | QDir::Reversed);
    for (auto& f : files) {
        f = dir.filePath(f);
    }
    return files;
}

void AutoSaver::prune(const QString& name) const {
    const auto files = autosaveFiles(name);
    for (qsizetype i = std::max(0, m_settings.keep); i < files.size(); ++i) {
        QFile::remove(files[i]);
    }
}

std::vector<AutoSaver::Recovery> AutoSaver::findOrphans(const QString& folder) {
    std::vector<Recovery> out;
    QDir dir(folder);
    for (const auto& name : dir.entryList({QStringLiteral("session-*.loki")}, QDir::Files)) {
        const QString file = dir.filePath(name);
        const QString lockPath = file.left(file.size() - 5) + QStringLiteral(".lock");
        // tryLock succeeds only when no live process holds it (a crashed owner's lock is stale).
        QLockFile lock(lockPath);
        if (!lock.tryLock(0)) {
            continue; // a running Loki owns this session
        }
        lock.unlock();
        Recovery r;
        r.file = file;
        QFile meta(metaFileFor(file));
        if (meta.open(QIODevice::ReadOnly)) {
            const auto obj = QJsonDocument::fromJson(meta.readAll()).object();
            r.originalPath = obj.value(QStringLiteral("originalPath")).toString();
            r.saved = QDateTime::fromString(obj.value(QStringLiteral("saved")).toString(), Qt::ISODate);
        }
        out.push_back(r);
    }
    return out;
}

void AutoSaver::discard(const Recovery& recovery) {
    QFile::remove(recovery.file);
    QFile::remove(metaFileFor(recovery.file));
    QFile::remove(recovery.file.left(recovery.file.size() - 5) + QStringLiteral(".lock"));
}

QString AutoSaver::keepAsAutosave(const Recovery& recovery) {
    const QFileInfo info(recovery.file);
    const QString name = recovery.originalPath.isEmpty() ? QStringLiteral("Untitled")
                                                         : QFileInfo(recovery.originalPath).completeBaseName();
    const QString stamp = (recovery.saved.isValid() ? recovery.saved : info.lastModified()).toString(QStringLiteral("yyyyMMdd-HHmmss"));
    const QString target = info.dir().filePath(QStringLiteral("%1-recovered-%2.loki").arg(name, stamp));
    QFile::remove(target);
    const bool moved = QFile::rename(recovery.file, target);
    if (!moved) {
        spdlog::warn("Could not keep recovery file {}", recovery.file.toStdString());
        return {};
    }
    discard(recovery); // the remaining .json / .lock
    return target;
}

AutoSaver::Settings AutoSaver::loadSettings() {
    QSettings s;
    Settings out;
    out.enabled = s.value(QStringLiteral("autosave/enabled"), out.enabled).toBool();
    out.intervalMinutes = s.value(QStringLiteral("autosave/intervalMinutes"), out.intervalMinutes).toInt();
    out.keep = s.value(QStringLiteral("autosave/keep"), out.keep).toInt();
    return out;
}

void AutoSaver::saveSettings(const Settings& settings) {
    QSettings s;
    s.setValue(QStringLiteral("autosave/enabled"), settings.enabled);
    s.setValue(QStringLiteral("autosave/intervalMinutes"), settings.intervalMinutes);
    s.setValue(QStringLiteral("autosave/keep"), settings.keep);
}

} // namespace loki::ui
