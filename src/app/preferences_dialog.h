#pragma once

#include "playback/performance.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QSpinBox;

namespace loki::ui {
class AutoSaver;
class MediaServices;
} // namespace loki::ui

namespace loki::app {

// Preferences: media cache location (thumbnails, waveforms, later proxies) and cache cleanup.
class PreferencesDialog final : public QDialog {
    Q_OBJECT

public:
    // `autoSaver` may be null (its settings are then only stored).
    explicit PreferencesDialog(ui::MediaServices& services, ui::AutoSaver* autoSaver = nullptr, QWidget* parent = nullptr);

    // Stored playback performance mode ("performance/mode"; used at start-up).
    [[nodiscard]] static playback::PerformanceMode performanceMode();
    static void setPerformanceMode(playback::PerformanceMode mode);

private:
    void accept() override;
    void refreshUsage();

    ui::MediaServices& m_services;
    QLineEdit* m_cacheDir = nullptr;
    QLabel* m_usage = nullptr;
    ui::AutoSaver* m_autoSaver = nullptr;
    QCheckBox* m_autosave = nullptr;
    QSpinBox* m_interval = nullptr;
    QSpinBox* m_keep = nullptr;
    QComboBox* m_performance = nullptr;
};

} // namespace loki::app
