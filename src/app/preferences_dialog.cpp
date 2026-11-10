#include "app/preferences_dialog.h"

#include "media/media_cache.h"
#include "ui/auto_saver.h"
#include "ui/media_services.h"

#include <QCheckBox>
#include <QComboBox>
#include <QSettings>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace loki::app {

PreferencesDialog::PreferencesDialog(ui::MediaServices& services, ui::AutoSaver* autoSaver, QWidget* parent)
    : QDialog(parent), m_services(services), m_autoSaver(autoSaver) {
    setWindowTitle(tr("Preferences"));
    setMinimumWidth(520);

    m_cacheDir = new QLineEdit(services.cacheDirectory(), this);
    auto* browse = new QPushButton(tr("Browse..."), this);
    auto* row = new QHBoxLayout();
    row->addWidget(m_cacheDir, 1);
    row->addWidget(browse);

    m_usage = new QLabel(this);
    auto* clear = new QPushButton(tr("Clean Cache"), this);
    auto* usageRow = new QHBoxLayout();
    usageRow->addWidget(m_usage, 1);
    usageRow->addWidget(clear);

    auto* form = new QFormLayout();
    form->addRow(tr("Media Cache folder:"), row);
    form->addRow(QString(), usageRow);

    // Autosave (D25).
    const auto autosave = ui::AutoSaver::loadSettings();
    m_autosave = new QCheckBox(tr("Automatically save projects and keep a crash-recovery copy"), this);
    m_autosave->setChecked(autosave.enabled);
    m_interval = new QSpinBox(this);
    m_interval->setRange(1, 120);
    m_interval->setSuffix(tr(" min"));
    m_interval->setValue(autosave.intervalMinutes);
    m_keep = new QSpinBox(this);
    m_keep->setRange(1, 100);
    m_keep->setValue(autosave.keep);
    form->addRow(tr("Auto Save:"), m_autosave);
    form->addRow(tr("Save every:"), m_interval);
    form->addRow(tr("Versions to keep:"), m_keep);

    // Performance: Standard suits a 2-core laptop; High uses more cores and memory.
    const auto hardware = playback::detectHardware();
    const auto detected = playback::performanceProfile(playback::PerformanceMode::Auto, hardware);
    m_performance = new QComboBox(this);
    m_performance->addItem(tr("Auto (this computer: %1)")
                               .arg(detected.mode == playback::PerformanceMode::High ? tr("High") : tr("Standard")),
                           static_cast<int>(playback::PerformanceMode::Auto));
    m_performance->addItem(tr("Standard (2-4 cores, 8 GB)"), static_cast<int>(playback::PerformanceMode::Standard));
    m_performance->addItem(tr("High (8+ threads, 12+ GB: more layers, bigger caches, parallel decoding)"),
                           static_cast<int>(playback::PerformanceMode::High));
    m_performance->setCurrentIndex(std::max(0, m_performance->findData(static_cast<int>(performanceMode()))));
    auto* hardwareLabel = new QLabel(tr("%1 threads, %2 GB RAM. Changes apply when Loki restarts.")
                                         .arg(hardware.threads)
                                         .arg(static_cast<double>(hardware.ramBytes) / (1024.0 * 1024.0 * 1024.0), 0, 'f', 1),
                                     this);
    hardwareLabel->setWordWrap(true);
    form->addRow(tr("Playback performance:"), m_performance);
    form->addRow(QString(), hardwareLabel);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addStretch();
    layout->addWidget(buttons);

    connect(browse, &QPushButton::clicked, this, [this] {
        const auto dir = QFileDialog::getExistingDirectory(this, tr("Media Cache folder"), m_cacheDir->text());
        if (!dir.isEmpty()) {
            m_cacheDir->setText(QDir::toNativeSeparators(dir));
        }
    });
    connect(clear, &QPushButton::clicked, this, [this] {
        const media::MediaCache cache(m_services.cacheDirectory().toStdWString());
        if (!cache.clear()) {
            QMessageBox::warning(this, tr("Clean Cache"), tr("Some cache files are in use and were not removed."));
        }
        m_services.setCacheDirectory(m_services.cacheDirectory()); // drop in-memory copies too
        refreshUsage();
    });
    connect(buttons, &QDialogButtonBox::accepted, this, &PreferencesDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    refreshUsage();
}

playback::PerformanceMode PreferencesDialog::performanceMode() {
    const int value = QSettings().value(QStringLiteral("performance/mode"), 0).toInt();
    return value == 1 ? playback::PerformanceMode::Standard
           : value == 2 ? playback::PerformanceMode::High
                        : playback::PerformanceMode::Auto;
}

void PreferencesDialog::setPerformanceMode(playback::PerformanceMode mode) {
    QSettings().setValue(QStringLiteral("performance/mode"), static_cast<int>(mode));
}

void PreferencesDialog::refreshUsage() {
    const media::MediaCache cache(m_services.cacheDirectory().toStdWString());
    m_usage->setText(tr("In use: %1").arg(QLocale().formattedDataSize(static_cast<qint64>(cache.diskUsage()))));
}

void PreferencesDialog::accept() {
    const auto dir = QDir::cleanPath(m_cacheDir->text().trimmed());
    if (dir.isEmpty() || !QDir().mkpath(dir)) {
        QMessageBox::warning(this, tr("Preferences"), tr("The Media Cache folder cannot be created."));
        return;
    }
    setPerformanceMode(static_cast<playback::PerformanceMode>(m_performance->currentData().toInt()));
    ui::AutoSaver::Settings autosave;
    autosave.enabled = m_autosave->isChecked();
    autosave.intervalMinutes = m_interval->value();
    autosave.keep = m_keep->value();
    ui::AutoSaver::saveSettings(autosave);
    if (m_autoSaver != nullptr) {
        m_autoSaver->setSettings(autosave);
    }
    if (dir != QDir::cleanPath(m_services.cacheDirectory())) {
        ui::setConfiguredCacheDirectory(dir);
        m_services.setCacheDirectory(dir);
    }
    QDialog::accept();
}

} // namespace loki::app
