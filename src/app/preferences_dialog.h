#pragma once

#include <QDialog>

class QLabel;
class QLineEdit;

namespace loki::ui {
class MediaServices;
} // namespace loki::ui

namespace loki::app {

// Preferences: media cache location (thumbnails, waveforms, later proxies) and cache cleanup.
class PreferencesDialog final : public QDialog {
    Q_OBJECT

public:
    explicit PreferencesDialog(ui::MediaServices& services, QWidget* parent = nullptr);

private:
    void accept() override;
    void refreshUsage();

    ui::MediaServices& m_services;
    QLineEdit* m_cacheDir = nullptr;
    QLabel* m_usage = nullptr;
};

} // namespace loki::app
