#include "ui/export_queue.h"

#include <QApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QOffscreenSurface>
#include <QPushButton>
#include <QSurfaceFormat>
#include <QThread>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <system_error>

namespace loki::ui {

namespace {

constexpr int kIdRole = Qt::UserRole + 1;

QString stateText(ExportQueue::State s) {
    switch (s) {
    case ExportQueue::State::Waiting: return QObject::tr("Waiting");
    case ExportQueue::State::Running: return QObject::tr("Running");
    case ExportQueue::State::Finished: return QObject::tr("Done");
    case ExportQueue::State::Failed: return QObject::tr("Failed");
    case ExportQueue::State::Cancelled: return QObject::tr("Cancelled");
    }
    return {};
}

} // namespace

ExportQueue::ExportQueue(QObject* parent) : QObject(parent) {
    // The offscreen surface must be created on the GUI thread; each job's GL context uses it.
    m_surface = new QOffscreenSurface(nullptr, this);
    QSurfaceFormat format;
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CompatibilityProfile);
    m_surface->setFormat(format);
    m_surface->create();
    m_timer = new QTimer(this);
    m_timer->setInterval(150);
    connect(m_timer, &QTimer::timeout, this, &ExportQueue::poll);
}

ExportQueue::~ExportQueue() {
    if (m_job) {
        m_job->cancel();
    }
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

int ExportQueue::enqueue(QString name, exporter::ExportRequest request, std::filesystem::path finalFile, bool preview) {
    Entry e;
    e.id = m_nextId++;
    e.name = std::move(name);
    e.request = std::move(request);
    e.finalFile = std::move(finalFile);
    e.preview = preview;
    m_entries.push_back(std::move(e));
    emit changed();
    if (!m_job) {
        startNext();
    }
    return m_entries.back().id;
}

void ExportQueue::startNext() {
    for (auto& e : m_entries) {
        if (e.state != State::Waiting) {
            continue;
        }
        std::vector<std::unique_ptr<exporter::FrameRenderer>> renderers;
        if (!m_forceCpu) {
            renderers.push_back(exporter::makeGlRenderer(m_surface));
        }
        renderers.push_back(exporter::makeCpuRenderer());
        if (!e.finalFile.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(e.finalFile.parent_path(), ec);
        }
        m_job = std::make_unique<exporter::ExportJob>(e.request, std::move(renderers));
        m_runningId = e.id;
        e.state = State::Running;
        m_done = false;
        auto* job = m_job.get();
        m_thread = std::jthread([this, job] {
            m_result = job->run();
            m_done = true;
        });
        m_timer->start();
        emit changed();
        return;
    }
    m_timer->stop();
}

void ExportQueue::poll() {
    if (!m_job) {
        return;
    }
    for (auto& e : m_entries) {
        if (e.id == m_runningId) {
            e.progress = m_job->progress();
        }
    }
    if (m_done) {
        finishCurrent();
    }
    emit changed();
}

void ExportQueue::finishCurrent() {
    if (m_thread.joinable()) {
        m_thread.join();
    }
    m_job.reset();
    for (auto& e : m_entries) {
        if (e.id != m_runningId) {
            continue;
        }
        bool ok = m_result.status == exporter::ExportStatus::Finished;
        if (ok && !e.finalFile.empty()) {
            std::error_code ec;
            std::filesystem::rename(e.request.file, e.finalFile, ec);
            ok = !ec;
            if (ec) {
                m_result.error = ec.message();
            }
        }
        e.state = ok ? State::Finished
                     : m_result.status == exporter::ExportStatus::Cancelled ? State::Cancelled : State::Failed;
        e.progress = ok ? 1.0 : e.progress;
        e.message = ok ? tr("%1 frames in %2 s").arg(m_result.frames).arg(m_result.seconds, 0, 'f', 1)
                       : QString::fromStdString(m_result.error);
        emit entryFinished(e.id, ok, e.message, e.preview);
    }
    m_runningId = 0;
    startNext();
    emit changed();
}

void ExportQueue::cancel(int id) {
    for (auto& e : m_entries) {
        if (e.id != id) {
            continue;
        }
        if (e.state == State::Waiting) {
            e.state = State::Cancelled;
        } else if (e.state == State::Running && m_job) {
            m_job->cancel();
        }
    }
    emit changed();
}

void ExportQueue::cancelAll() {
    for (const auto& e : m_entries) {
        cancel(e.id);
    }
}

void ExportQueue::clearFinished() {
    std::erase_if(m_entries, [](const Entry& e) { return e.state != State::Waiting && e.state != State::Running; });
    emit changed();
}

void ExportQueue::waitForIdle() {
    while (m_job) {
        if (m_thread.joinable()) {
            m_thread.join();
        }
        finishCurrent();
    }
}

// --- Panel ---------------------------------------------------------------------------------------------

ExportQueuePanel::ExportQueuePanel(ExportQueue& queue, QWidget* parent) : QWidget(parent), m_queue(queue) {
    m_list = new QTreeWidget;
    m_list->setColumnCount(3);
    m_list->setHeaderLabels({tr("Export"), tr("Status"), tr("Details")});
    m_list->setRootIsDecorated(false);
    m_list->header()->setStretchLastSection(true);
    auto* cancel = new QPushButton(tr("Cancel"));
    auto* clear = new QPushButton(tr("Clear Finished"));
    auto* buttons = new QHBoxLayout;
    buttons->addStretch(1);
    buttons->addWidget(cancel);
    buttons->addWidget(clear);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->addWidget(m_list, 1);
    layout->addLayout(buttons);
    connect(cancel, &QPushButton::clicked, this, [this] {
        if (const auto* item = m_list->currentItem()) {
            m_queue.cancel(item->data(0, kIdRole).toInt());
        }
    });
    connect(clear, &QPushButton::clicked, this, [this] { m_queue.clearFinished(); });
    connect(&m_queue, &ExportQueue::changed, this, &ExportQueuePanel::rebuild);
    rebuild();
}

void ExportQueuePanel::rebuild() {
    const auto* current = m_list->currentItem();
    const int selected = current != nullptr ? current->data(0, kIdRole).toInt() : 0;
    m_list->clear();
    for (const auto& e : m_queue.entries()) {
        auto* item = new QTreeWidgetItem(m_list);
        item->setText(0, e.name);
        item->setText(1, e.state == ExportQueue::State::Running
                             ? tr("Running %1 %").arg(static_cast<int>(e.progress * 100.0))
                             : stateText(e.state));
        item->setText(2, e.message);
        item->setData(0, kIdRole, e.id);
        if (e.id == selected) {
            m_list->setCurrentItem(item);
        }
    }
}

} // namespace loki::ui
