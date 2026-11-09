#include "ui/timeline/timeline_panel.h"

#include "ui/project_controller.h"

#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollBar>
#include <QSlider>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <cmath>

namespace loki::ui::timeline {

namespace {

// Zoom slider is logarithmic: value v -> 2^(v/10) pixels per second (0.5 .. 4096).
int sliderFromPps(double pps) {
    return static_cast<int>(std::lround(std::log2(pps) * 10.0));
}
double ppsFromSlider(int value) {
    return std::pow(2.0, value / 10.0);
}

} // namespace

TimelinePanel::TimelinePanel(ProjectController& controller, MediaServices& services, QWidget* parent)
    : QWidget(parent) {
    m_view = new TimelineView(controller, services, this);
    m_title = new QLabel(this);
    m_title->setContentsMargins(6, 0, 6, 0);

    const auto toolButton = [this](const QString& text, const QString& tip) {
        auto* b = new QToolButton(this);
        b->setText(text);
        b->setToolTip(tip);
        b->setCheckable(true);
        b->setAutoRaise(true);
        return b;
    };
    m_selectTool = toolButton(tr("Select"), tr("Selection Tool (V)"));
    m_razorTool = toolButton(tr("Razor"), tr("Razor Tool (C) - Shift+click cuts all tracks"));
    m_snap = toolButton(tr("Snap"), tr("Snap in Timeline (S)"));
    m_selectTool->setChecked(true);
    m_snap->setChecked(true);
    m_zoom = new QSlider(Qt::Horizontal, this);
    m_zoom->setRange(sliderFromPps(0.5), sliderFromPps(4000));
    m_zoom->setValue(sliderFromPps(m_view->metrics().pixelsPerSecond));
    m_zoom->setMaximumWidth(160);
    m_zoom->setToolTip(tr("Zoom (+ / -, \\ to fit, Alt+wheel)"));

    auto* bar = new QHBoxLayout();
    bar->setContentsMargins(4, 2, 4, 2);
    bar->addWidget(m_title);
    bar->addStretch();
    bar->addWidget(m_selectTool);
    bar->addWidget(m_razorTool);
    bar->addSpacing(12);
    bar->addWidget(m_snap);
    bar->addSpacing(12);
    bar->addWidget(new QLabel(tr("Zoom"), this));
    bar->addWidget(m_zoom);

    m_hScroll = new QScrollBar(Qt::Horizontal, this);
    m_vScroll = new QScrollBar(Qt::Vertical, this);
    auto* grid = new QGridLayout();
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(0);
    grid->addWidget(m_view, 0, 0);
    grid->addWidget(m_vScroll, 0, 1);
    grid->addWidget(m_hScroll, 1, 0);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(bar);
    layout->addLayout(grid, 1);

    connect(m_selectTool, &QToolButton::clicked, this, [this] { m_view->setTool(Tool::Selection); });
    connect(m_razorTool, &QToolButton::clicked, this, [this] { m_view->setTool(Tool::Razor); });
    connect(m_snap, &QToolButton::toggled, m_view, &TimelineView::setSnapping);
    connect(m_view, &TimelineView::toolChanged, this, [this](Tool tool) {
        m_selectTool->setChecked(tool == Tool::Selection);
        m_razorTool->setChecked(tool == Tool::Razor);
    });
    connect(m_view, &TimelineView::snappingChanged, m_snap, &QToolButton::setChecked);
    connect(m_zoom, &QSlider::valueChanged, this, [this](int v) {
        if (!m_syncing) {
            m_view->setPixelsPerSecond(ppsFromSlider(v));
        }
    });
    connect(m_view, &TimelineView::zoomChanged, this, [this](double pps) {
        m_syncing = true;
        m_zoom->setValue(sliderFromPps(pps));
        m_syncing = false;
        syncScrollBars();
    });
    connect(m_view, &TimelineView::scrollChanged, this, &TimelinePanel::syncScrollBars);
    connect(m_hScroll, &QScrollBar::valueChanged, this, [this](int v) {
        if (!m_syncing) {
            m_view->metrics().scrollX = v;
            m_view->update();
        }
    });
    connect(m_vScroll, &QScrollBar::valueChanged, this, [this](int v) {
        if (!m_syncing) {
            m_view->metrics().scrollY = v;
            m_view->update();
        }
    });
    connect(m_view, &TimelineView::statusMessage, this, &TimelinePanel::statusMessage);
    connect(&controller, &ProjectController::projectChanged, this, [this](const model::ChangeEvent& e) {
        if (e.kind == model::ChangeKind::SequenceChanged && e.id == m_view->sequenceId().uuid()) {
            if (const auto* seq = m_view->sequence()) {
                m_title->setText(QString::fromStdString(seq->name));
            }
        }
        syncScrollBars();
    });

    m_timer = new QTimer(this);
    m_timer->setInterval(30);
    connect(m_timer, &QTimer::timeout, this, [this] {
        m_view->refreshPlayhead();
        syncScrollBars();
    });
    m_timer->start();
}

void TimelinePanel::setSequence(model::SequenceId id) {
    m_view->setSequence(id);
    const auto* seq = m_view->sequence();
    m_title->setText(seq != nullptr ? QString::fromStdString(seq->name) : tr("(no sequence)"));
    syncScrollBars();
}

void TimelinePanel::setTransport(Transport transport) {
    m_view->setTransport(std::move(transport));
}

void TimelinePanel::syncScrollBars() {
    m_syncing = true;
    const auto& m = m_view->metrics();
    const int visibleW = std::max(1, m_view->width() - TimelineView::kHeaderWidth);
    const int contentW = static_cast<int>(m_view->contentWidthPixels());
    m_hScroll->setRange(0, std::max(0, contentW - visibleW));
    m_hScroll->setPageStep(visibleW);
    m_hScroll->setSingleStep(40);
    m_hScroll->setValue(static_cast<int>(m.scrollX));
    const auto* seq = m_view->sequence();
    const int contentH = seq != nullptr ? contentHeight(*seq, m) : 0;
    m_vScroll->setRange(0, std::max(0, contentH - m_view->height()));
    m_vScroll->setPageStep(std::max(1, m_view->height()));
    m_vScroll->setValue(m.scrollY);
    m_syncing = false;
}

} // namespace loki::ui::timeline
