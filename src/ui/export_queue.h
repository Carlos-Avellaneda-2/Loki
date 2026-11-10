#pragma once

#include "export/export_job.h"

#include <QObject>
#include <QString>
#include <QWidget>

#include <atomic>
#include <deque>
#include <filesystem>
#include <memory>
#include <thread>

class QOffscreenSurface;
class QTimer;
class QTreeWidget;

namespace loki::ui {

// Export queue (D41): exports run one after the other on a worker thread while editing continues.
// Also renders the render previews (D42). GPU rendering with the CPU compositor as fallback.
class ExportQueue final : public QObject {
    Q_OBJECT

public:
    enum class State { Waiting, Running, Finished, Failed, Cancelled };
    struct Entry {
        int id = 0;
        QString name;
        exporter::ExportRequest request;
        // When set, the file is written to request.file and renamed to this on success (render
        // previews: a half-written preview is never picked up by playback).
        std::filesystem::path finalFile;
        State state = State::Waiting;
        double progress = 0.0;
        QString message;
        bool preview = false;
    };

    explicit ExportQueue(QObject* parent = nullptr);
    ~ExportQueue() override;

    ExportQueue(const ExportQueue&) = delete;
    ExportQueue& operator=(const ExportQueue&) = delete;
    ExportQueue(ExportQueue&&) = delete;
    ExportQueue& operator=(ExportQueue&&) = delete;

    int enqueue(QString name, exporter::ExportRequest request, std::filesystem::path finalFile = {}, bool preview = false);
    void cancel(int id);
    void cancelAll();
    void clearFinished();
    [[nodiscard]] const std::deque<Entry>& entries() const noexcept { return m_entries; }
    [[nodiscard]] bool busy() const noexcept { return m_job != nullptr; }
    void setForceCpu(bool on) noexcept { m_forceCpu = on; }
    // Blocks until nothing is waiting or running (tests), processing finished jobs.
    void waitForIdle();

signals:
    void changed();
    void entryFinished(int id, bool ok, const QString& message, bool preview);

private:
    void startNext();
    void poll();
    void finishCurrent();

    std::deque<Entry> m_entries;
    int m_nextId = 1;
    QOffscreenSurface* m_surface = nullptr;
    QTimer* m_timer = nullptr;
    std::unique_ptr<exporter::ExportJob> m_job;
    int m_runningId = 0;
    std::jthread m_thread;
    std::atomic<bool> m_done{false};
    exporter::ExportResult m_result;
    bool m_forceCpu = false;
};

// Panel listing the queue with progress, Cancel and Clear Finished.
class ExportQueuePanel final : public QWidget {
    Q_OBJECT

public:
    explicit ExportQueuePanel(ExportQueue& queue, QWidget* parent = nullptr);

private:
    void rebuild();

    ExportQueue& m_queue;
    QTreeWidget* m_list = nullptr;
};

} // namespace loki::ui
