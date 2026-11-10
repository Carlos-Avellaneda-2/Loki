#include "ui/timeline/timeline_panel.h"

#include "model/transitions.h"
#include "ui/project_controller.h"
#include "ui/shortcut_manager.h"

#include <QComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollBar>
#include <QSlider>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <cmath>
#include <functional>

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
    : QWidget(parent), m_controller(controller) {
    m_view = new TimelineView(controller, services, this);
    m_sequences = new QComboBox(this);
    m_sequences->setMinimumContentsLength(14);
    m_sequences->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_sequences->setToolTip(tr("Sequence shown in the Timeline and Program Monitor"));
    connect(m_sequences, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (!m_refreshing && index >= 0) {
            if (const auto uuid = Uuid::parse(m_sequences->itemData(index).toString().toStdString())) {
                emit sequenceRequested(model::SequenceId(*uuid));
            }
        }
    });

    const auto toolButton = [this](const QString& text, const QString& tip) {
        auto* b = new QToolButton(this);
        b->setText(text);
        b->setToolTip(tip);
        b->setCheckable(true);
        b->setAutoRaise(true);
        return b;
    };
    const std::pair<Tool, std::pair<QString, QString>> tools[] = {
        {Tool::Selection, {tr("Select"), tr("Selection Tool (V)")}},
        {Tool::Ripple, {tr("Ripple"), tr("Ripple Edit Tool (B): trim and close the gap")}},
        {Tool::Roll, {tr("Roll"), tr("Rolling Edit Tool (N): move an edit point")}},
        {Tool::Razor, {tr("Razor"), tr("Razor Tool (C) - Shift+click cuts all tracks")}},
        {Tool::Slip, {tr("Slip"), tr("Slip Tool (Y): change the source range, keep position")}},
        {Tool::Slide, {tr("Slide"), tr("Slide Tool (U): move a clip between its neighbours")}},
        {Tool::RateStretch, {tr("Stretch"), tr("Rate Stretch Tool (R): drag an edge to change the speed")}},
    };
    for (const auto& [tool, labels] : tools) {
        auto* b = toolButton(labels.first, labels.second);
        b->setChecked(tool == Tool::Selection);
        connect(b, &QToolButton::clicked, this, [this, tool = tool] { m_view->setTool(tool); });
        m_toolButtons.emplace_back(tool, b);
    }
    m_snap = toolButton(tr("Snap"), tr("Snap in Timeline (S)"));
    m_snap->setChecked(true);
    m_zoom = new QSlider(Qt::Horizontal, this);
    m_zoom->setRange(sliderFromPps(0.5), sliderFromPps(4000));
    m_zoom->setValue(sliderFromPps(m_view->metrics().pixelsPerSecond));
    m_zoom->setMaximumWidth(160);
    m_zoom->setToolTip(tr("Zoom (+ / -, \\ to fit, Alt+wheel)"));

    auto* bar = new QHBoxLayout();
    bar->setContentsMargins(4, 2, 4, 2);
    bar->addWidget(m_sequences);
    bar->addStretch();
    for (const auto& [tool, button] : m_toolButtons) {
        bar->addWidget(button);
    }
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

    connect(m_snap, &QToolButton::toggled, m_view, &TimelineView::setSnapping);
    connect(m_view, &TimelineView::toolChanged, this, [this](Tool tool) {
        for (const auto& [t, button] : m_toolButtons) {
            button->setChecked(t == tool);
        }
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
        if (e.kind == model::ChangeKind::SequenceChanged || e.kind == model::ChangeKind::SequenceAdded ||
            e.kind == model::ChangeKind::SequenceRemoved) {
            refreshSequences();
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
    refreshSequences();
    syncScrollBars();
}

void TimelinePanel::refreshSequences() {
    m_refreshing = true;
    m_sequences->clear();
    for (const auto& seq : m_controller.project().sequences()) {
        m_sequences->addItem(QString::fromStdString(seq.name), QString::fromStdString(seq.id.toString()));
    }
    m_sequences->setCurrentIndex(m_sequences->findData(QString::fromStdString(m_view->sequenceId().toString())));
    m_refreshing = false;
}

void TimelinePanel::setTransport(Transport transport) {
    m_view->setTransport(std::move(transport));
}

void TimelinePanel::registerShortcuts(ShortcutManager& s) {
    const QString ctx = QStringLiteral("Timeline");
    auto* v = m_view;
    const auto add = [&](const char* id, const QString& text, const QKeySequence& key, std::function<void()> fn) {
        s.create(v, QStringLiteral("timeline.") + QLatin1String(id), ctx, text, key, std::move(fn));
    };
    add("tool.selection", tr("Selection Tool"), QKeySequence(Qt::Key_V), [v] { v->setTool(Tool::Selection); });
    add("tool.razor", tr("Razor Tool"), QKeySequence(Qt::Key_C), [v] { v->setTool(Tool::Razor); });
    add("tool.ripple", tr("Ripple Edit Tool"), QKeySequence(Qt::Key_B), [v] { v->setTool(Tool::Ripple); });
    add("tool.roll", tr("Rolling Edit Tool"), QKeySequence(Qt::Key_N), [v] { v->setTool(Tool::Roll); });
    add("tool.slip", tr("Slip Tool"), QKeySequence(Qt::Key_Y), [v] { v->setTool(Tool::Slip); });
    add("tool.slide", tr("Slide Tool"), QKeySequence(Qt::Key_U), [v] { v->setTool(Tool::Slide); });
    add("tool.rateStretch", tr("Rate Stretch Tool"), QKeySequence(Qt::Key_R), [v] { v->setTool(Tool::RateStretch); });
    add("speedDuration", tr("Speed/Duration..."), QKeySequence(Qt::CTRL | Qt::Key_R), [v] { v->speedDuration(); });
    add("frameHold", tr("Add Frame Hold"), QKeySequence(Qt::SHIFT | Qt::Key_H), [v] { v->addFrameHold(); });
    add("nest", tr("Nest..."), QKeySequence(), [v] { v->nestSelection(); });
    add("addMarker", tr("Add Marker"), QKeySequence(Qt::Key_M), [v] { v->addMarker(); });
    add("nextMarker", tr("Go to Next Marker"), QKeySequence(Qt::SHIFT | Qt::Key_M), [v] { v->goToMarker(true); });
    add("prevMarker", tr("Go to Previous Marker"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_M),
        [v] { v->goToMarker(false); });
    add("clearMarker", tr("Clear Current Marker"), QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_M),
        [v] { v->clearMarkerAtPlayhead(); });
    add("snap", tr("Snap in Timeline"), QKeySequence(Qt::Key_S), [v] { v->setSnapping(!v->snapping()); });
    add("delete", tr("Clear (Delete)"), QKeySequence(Qt::Key_Delete), [v] { v->deleteSelection(); });
    add("delete.alt", tr("Clear (Backspace)"), QKeySequence(Qt::Key_Backspace), [v] { v->deleteSelection(); });
    add("rippleDelete", tr("Ripple Delete"), QKeySequence(Qt::SHIFT | Qt::Key_Delete), [v] { v->rippleDeleteSelection(); });
    add("addEdit", tr("Add Edit"), QKeySequence(Qt::CTRL | Qt::Key_K), [v] { v->razorAtPlayhead(false); });
    add("addEditAll", tr("Add Edit to All Tracks"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_K),
        [v] { v->razorAtPlayhead(true); });
    add("copy", tr("Copy"), QKeySequence(Qt::CTRL | Qt::Key_C), [v] { v->copySelection(); });
    add("cut", tr("Cut"), QKeySequence(Qt::CTRL | Qt::Key_X), [v] { v->cutSelection(); });
    add("paste", tr("Paste"), QKeySequence(Qt::CTRL | Qt::Key_V), [v] { v->paste(false); });
    add("pasteInsert", tr("Paste Insert"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_V), [v] { v->paste(true); });
    add("defaultVideoTransition", tr("Apply Video Transition"), QKeySequence(Qt::CTRL | Qt::Key_D),
        [v] { v->applyTransitionAtPlayhead(std::string(model::kCrossDissolve)); });
    add("defaultAudioTransition", tr("Apply Audio Transition"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_D),
        [v] { v->applyTransitionAtPlayhead(std::string(model::kCrossfade)); });
    add("link", tr("Link / Unlink"), QKeySequence(Qt::CTRL | Qt::Key_L), [v] { v->toggleLinkSelection(); });
    add("zoomIn", tr("Zoom In"), QKeySequence(Qt::Key_Equal), [v] { v->setPixelsPerSecond(v->metrics().pixelsPerSecond * 1.5); });
    add("zoomOut", tr("Zoom Out"), QKeySequence(Qt::Key_Minus), [v] { v->setPixelsPerSecond(v->metrics().pixelsPerSecond / 1.5); });
    add("zoomFit", tr("Zoom to Sequence"), QKeySequence(Qt::Key_Backslash), [v] { v->zoomToFit(); });
    add("play", tr("Play-Stop Toggle"), QKeySequence(Qt::Key_Space), [v] { v->togglePlay(); });
    add("stepBack", tr("Step Back 1 Frame"), QKeySequence(Qt::Key_Left), [v] { v->stepPlayhead(-1); });
    add("stepForward", tr("Step Forward 1 Frame"), QKeySequence(Qt::Key_Right), [v] { v->stepPlayhead(1); });
    add("stepBack5", tr("Step Back 5 Frames"), QKeySequence(Qt::SHIFT | Qt::Key_Left), [v] { v->stepPlayhead(-5); });
    add("stepForward5", tr("Step Forward 5 Frames"), QKeySequence(Qt::SHIFT | Qt::Key_Right), [v] { v->stepPlayhead(5); });
    add("prevEdit", tr("Go to Previous Edit Point"), QKeySequence(Qt::Key_Up), [v] { v->goToEditPoint(false); });
    add("nextEdit", tr("Go to Next Edit Point"), QKeySequence(Qt::Key_Down), [v] { v->goToEditPoint(true); });
    add("start", tr("Go to Sequence Start"), QKeySequence(Qt::Key_Home), [v] { v->goToStart(); });
    add("end", tr("Go to Sequence End"), QKeySequence(Qt::Key_End), [v] { v->goToEnd(); });
    add("markIn", tr("Mark In"), QKeySequence(Qt::Key_I), [v] { v->markIn(); });
    add("markOut", tr("Mark Out"), QKeySequence(Qt::Key_O), [v] { v->markOut(); });
    add("goToIn", tr("Go to In"), QKeySequence(Qt::SHIFT | Qt::Key_I), [v] { v->goToIn(); });
    add("goToOut", tr("Go to Out"), QKeySequence(Qt::SHIFT | Qt::Key_O), [v] { v->goToOut(); });
    add("clearMarks", tr("Clear In and Out"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_X), [v] { v->clearMarks(); });
    add("lift", tr("Lift"), QKeySequence(Qt::Key_Semicolon), [v] { v->lift(); });
    add("extract", tr("Extract"), QKeySequence(Qt::Key_Apostrophe), [v] { v->extract(); });
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
