#pragma once

#include "export/export_job.h"
#include "model/ids.h"

#include <QDialog>

#include <memory>
#include <optional>
#include <thread>

class QComboBox;
class QLabel;
class QLineEdit;
class QOffscreenSurface;
class QProgressBar;
class QPushButton;
class QTimer;

namespace loki::ui {
class ExportQueue;
class ProjectController;
} // namespace loki::ui

namespace loki::app {

// File > Export > Media: preset, output file and range; the export runs on a worker thread with a
// progress bar and Cancel (the UI never blocks). GPU rendering with the CPU compositor as fallback.
class ExportDialog final : public QDialog {
    Q_OBJECT

public:
    ExportDialog(ui::ProjectController& controller, model::SequenceId sequence, QWidget* parent = nullptr);
    ~ExportDialog() override;

    ExportDialog(const ExportDialog&) = delete;
    ExportDialog& operator=(const ExportDialog&) = delete;
    ExportDialog(ExportDialog&&) = delete;
    ExportDialog& operator=(ExportDialog&&) = delete;

    // Public for tests.
    void setPreset(const std::string& id);
    void setOutputFile(const QString& file);
    void setUseInOut(bool on);
    void setForceCpu(bool on) { m_forceCpu = on; }
    // Starts the export (returns false with a message if the settings are invalid).
    bool startExport();
    [[nodiscard]] bool isRunning() const noexcept { return m_job != nullptr; }
    [[nodiscard]] const exporter::ExportResult& lastResult() const noexcept { return m_result; }
    // Waits for a running export (tests).
    void waitForFinished();
    // Export queue (D41): "Queue" adds the export there and closes the dialog.
    void setQueue(ui::ExportQueue* queue);
    bool addToQueue();
    // Captions (D40): 0 = none, 1 = burned into the picture, 2 = a .srt file next to the output.
    void setCaptionsMode(int mode);

signals:
    void exportFinished(bool ok, const QString& message);

private:
    void updateSummary();
    void poll();
    void finishJob();
    void reject() override;

    ui::ProjectController& m_controller;
    model::SequenceId m_sequence;
    QComboBox* m_preset = nullptr;
    QLineEdit* m_file = nullptr;
    QComboBox* m_range = nullptr;
    QComboBox* m_captions = nullptr;
    QPushButton* m_queueButton = nullptr;
    ui::ExportQueue* m_queue = nullptr;
    // Request from the current settings (nullopt with a message when invalid).
    std::optional<exporter::ExportRequest> buildRequest();
    QLabel* m_summary = nullptr;
    QProgressBar* m_progress = nullptr;
    QPushButton* m_export = nullptr;
    QTimer* m_timer = nullptr;
    QOffscreenSurface* m_surface = nullptr; // owned (Qt parent), created on the GUI thread
    std::unique_ptr<exporter::ExportJob> m_job;
    std::jthread m_thread;
    std::atomic<bool> m_done{false};
    exporter::ExportResult m_result;
    bool m_forceCpu = false;
};

} // namespace loki::app
