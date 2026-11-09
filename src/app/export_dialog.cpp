#include "app/export_dialog.h"

#include "ui/project_controller.h"
#include "util/timecode.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QOffscreenSurface>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSurfaceFormat>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace loki::app {

namespace {

QString qs(const std::string& s) {
    return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size()));
}

} // namespace

ExportDialog::ExportDialog(ui::ProjectController& controller, model::SequenceId sequence, QWidget* parent)
    : QDialog(parent), m_controller(controller), m_sequence(sequence) {
    setWindowTitle(tr("Export Media"));
    setMinimumWidth(520);
    m_preset = new QComboBox(this);
    for (const auto& p : exporter::exportPresets()) {
        m_preset->addItem(qs(p.name), qs(p.id));
    }
    m_file = new QLineEdit(this);
    auto* browse = new QToolButton(this);
    browse->setText(QStringLiteral("..."));
    m_range = new QComboBox(this);
    m_range->addItem(tr("Entire Sequence"));
    m_range->addItem(tr("Sequence In to Out"));
    m_summary = new QLabel(this);
    m_summary->setWordWrap(true);
    m_progress = new QProgressBar(this);
    m_progress->setRange(0, 1000);
    m_progress->setVisible(false);

    auto* fileRow = new QHBoxLayout();
    fileRow->addWidget(m_file, 1);
    fileRow->addWidget(browse);
    auto* form = new QFormLayout();
    form->addRow(tr("Preset"), m_preset);
    form->addRow(tr("Output File"), fileRow);
    form->addRow(tr("Range"), m_range);
    auto* buttons = new QDialogButtonBox(this);
    m_export = buttons->addButton(tr("Export"), QDialogButtonBox::AcceptRole);
    buttons->addButton(QDialogButtonBox::Cancel);
    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(m_summary);
    layout->addWidget(m_progress);
    layout->addWidget(buttons);

    const auto* seq = m_controller.project().findSequence(m_sequence);
    const bool marks = seq != nullptr && (seq->markIn || seq->markOut);
    m_range->setCurrentIndex(marks ? 1 : 0);
    QSettings settings;
    const auto lastPreset = settings.value(QStringLiteral("export/preset"), QStringLiteral("h264-match")).toString();
    m_preset->setCurrentIndex(std::max(0, m_preset->findData(lastPreset)));
    const QString folder = settings.value(QStringLiteral("export/folder"), QDir::homePath()).toString();
    // The sequence name may contain characters Windows does not allow in file names.
    QString base = seq != nullptr ? qs(seq->name).trimmed() : QString();
    base.replace(QRegularExpression(QStringLiteral(R"([\\/:*?"<>|])")), QStringLiteral("_"));
    if (base.isEmpty()) {
        base = QStringLiteral("export");
    }
    m_file->setText(QDir(folder).filePath(base + qs(exporter::exportPresets()[static_cast<std::size_t>(m_preset->currentIndex())].extension)));

    connect(m_preset, &QComboBox::currentIndexChanged, this, [this](int index) {
        // Keep the file name, switch the extension.
        const auto& preset = exporter::exportPresets()[static_cast<std::size_t>(index)];
        QFileInfo info(m_file->text());
        m_file->setText(QDir(info.absolutePath()).filePath(info.completeBaseName() + qs(preset.extension)));
        updateSummary();
    });
    connect(m_range, &QComboBox::currentIndexChanged, this, &ExportDialog::updateSummary);
    connect(browse, &QToolButton::clicked, this, [this] {
        const QString file = QFileDialog::getSaveFileName(this, tr("Export To"), m_file->text());
        if (!file.isEmpty()) {
            setOutputFile(file);
        }
    });
    connect(m_export, &QPushButton::clicked, this, [this] { (void)startExport(); });
    connect(buttons, &QDialogButtonBox::rejected, this, &ExportDialog::reject);

    // The offscreen surface must be created on the GUI thread; the job's GL context uses it.
    m_surface = new QOffscreenSurface(nullptr, this);
    QSurfaceFormat format;
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CompatibilityProfile);
    m_surface->setFormat(format);
    m_surface->create();

    m_timer = new QTimer(this);
    m_timer->setInterval(100);
    connect(m_timer, &QTimer::timeout, this, &ExportDialog::poll);
    updateSummary();
}

ExportDialog::~ExportDialog() {
    if (m_job) {
        m_job->cancel();
    }
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void ExportDialog::setPreset(const std::string& id) {
    m_preset->setCurrentIndex(std::max(0, m_preset->findData(qs(id))));
}

void ExportDialog::setOutputFile(const QString& file) {
    m_file->setText(file);
}

void ExportDialog::setUseInOut(bool on) {
    m_range->setCurrentIndex(on ? 1 : 0);
}

void ExportDialog::updateSummary() {
    const auto* seq = m_controller.project().findSequence(m_sequence);
    if (seq == nullptr) {
        m_summary->setText(tr("No active sequence."));
        m_export->setEnabled(false);
        return;
    }
    const auto& preset = exporter::exportPresets()[static_cast<std::size_t>(m_preset->currentIndex())];
    exporter::ExportRequest request;
    request.snapshot = std::make_shared<playback::SequenceSnapshot>(playback::SequenceSnapshot{*seq, {}, 0});
    request.preset = preset;
    const bool inOut = m_range->currentIndex() == 1;
    request.in = inOut ? seq->markIn.value_or(Rational()) : Rational();
    request.out = inOut ? seq->markOut.value_or(seq->duration()) : seq->duration();
    const auto [w, h] = exporter::ExportJob::outputSize(request);
    const auto rate = exporter::ExportJob::frameRate(request);
    QString text = tr("Duration %1 (%2 frames)")
                       .arg(qs(formatTimecode(timeToFrames(request.out - request.in, rate, Rounding::Ceil), rate,
                                              supportsDropFrame(rate))))
                       .arg(exporter::ExportJob::frameCount(request));
    if (preset.video != exporter::VideoCodec::None) {
        text += tr(" - %1 x %2 at %3 fps").arg(w).arg(h).arg(rate.toDouble(), 0, 'f', 3);
    }
    m_summary->setText(text);
    m_export->setEnabled(!isRunning() && exporter::ExportJob::frameCount(request) > 0);
}

bool ExportDialog::startExport() {
    if (isRunning()) {
        return false;
    }
    const auto* seq = m_controller.project().findSequence(m_sequence);
    const QString file = m_file->text().trimmed();
    if (seq == nullptr || file.isEmpty()) {
        QMessageBox::warning(this, windowTitle(), tr("Choose an output file."));
        return false;
    }
    exporter::ExportRequest request;
    request.snapshot = playback::makeSnapshot(m_controller.project(), m_sequence, 0);
    request.preset = exporter::exportPresets()[static_cast<std::size_t>(m_preset->currentIndex())];
    const bool inOut = m_range->currentIndex() == 1;
    request.in = inOut ? seq->markIn.value_or(Rational()) : Rational();
    request.out = inOut ? seq->markOut.value_or(seq->duration()) : seq->duration();
    request.file = std::filesystem::path(file.toStdWString());
    if (exporter::ExportJob::frameCount(request) <= 0) {
        QMessageBox::warning(this, windowTitle(), tr("The export range is empty."));
        return false;
    }
    QSettings settings;
    settings.setValue(QStringLiteral("export/preset"), qs(request.preset.id));
    settings.setValue(QStringLiteral("export/folder"), QFileInfo(file).absolutePath());

    std::vector<std::unique_ptr<exporter::FrameRenderer>> renderers;
    if (!m_forceCpu) {
        renderers.push_back(exporter::makeGlRenderer(m_surface));
    }
    renderers.push_back(exporter::makeCpuRenderer());
    m_job = std::make_unique<exporter::ExportJob>(std::move(request), std::move(renderers));
    m_done = false;
    m_result = {};
    m_progress->setValue(0);
    m_progress->setVisible(true);
    m_export->setEnabled(false);
    m_preset->setEnabled(false);
    m_range->setEnabled(false);
    m_file->setEnabled(false);
    auto* job = m_job.get();
    m_thread = std::jthread([this, job] {
        m_result = job->run();
        m_done = true;
    });
    m_timer->start();
    return true;
}

void ExportDialog::poll() {
    if (!m_job) {
        return;
    }
    m_progress->setValue(static_cast<int>(m_job->progress() * 1000));
    if (m_done) {
        finishJob();
    }
}

void ExportDialog::waitForFinished() {
    if (m_thread.joinable()) {
        m_thread.join();
    }
    if (m_job) {
        finishJob();
    }
}

void ExportDialog::finishJob() {
    m_timer->stop();
    if (m_thread.joinable()) {
        m_thread.join();
    }
    m_job.reset();
    m_preset->setEnabled(true);
    m_range->setEnabled(true);
    m_file->setEnabled(true);
    m_progress->setVisible(false);
    updateSummary();
    switch (m_result.status) {
    case exporter::ExportStatus::Finished:
        emit exportFinished(true, tr("Exported %1 frames in %2 s (%3, %4)")
                                      .arg(m_result.frames)
                                      .arg(m_result.seconds, 0, 'f', 1)
                                      .arg(m_result.renderer.empty() ? tr("audio only") : qs(m_result.renderer))
                                      .arg(m_result.videoEncoder.empty() ? tr("no video") : qs(m_result.videoEncoder)));
        if (isVisible()) {
            accept();
        }
        break;
    case exporter::ExportStatus::Cancelled: emit exportFinished(false, tr("Export cancelled")); break;
    default:
        emit exportFinished(false, tr("Export failed: %1").arg(qs(m_result.error)));
        if (isVisible()) {
            QMessageBox::warning(this, windowTitle(), tr("Export failed.\n\n%1").arg(qs(m_result.error)));
        }
        break;
    }
}

void ExportDialog::reject() {
    if (m_job) {
        m_job->cancel(); // Cancel stops the export first; the dialog closes when it has stopped
        return;
    }
    QDialog::reject();
}

} // namespace loki::app
