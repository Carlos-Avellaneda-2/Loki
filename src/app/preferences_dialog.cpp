#include "app/preferences_dialog.h"

#include "media/media_cache.h"
#include "ui/media_services.h"

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
#include <QVBoxLayout>

namespace loki::app {

PreferencesDialog::PreferencesDialog(ui::MediaServices& services, QWidget* parent)
    : QDialog(parent), m_services(services) {
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
    if (dir != QDir::cleanPath(m_services.cacheDirectory())) {
        ui::setConfiguredCacheDirectory(dir);
        m_services.setCacheDirectory(dir);
    }
    QDialog::accept();
}

} // namespace loki::app
