#pragma once

#include "ui/timeline/timeline_view.h"

#include <QWidget>

class QLabel;
class QScrollBar;
class QSlider;
class QTimer;
class QToolButton;

namespace loki::ui::timeline {

// Timeline panel: tool bar (Selection/Razor, snapping, zoom), the timeline view and scroll bars.
class TimelinePanel final : public QWidget {
    Q_OBJECT

public:
    TimelinePanel(ProjectController& controller, MediaServices& services, QWidget* parent = nullptr);

    [[nodiscard]] TimelineView* view() const noexcept { return m_view; }
    void setSequence(model::SequenceId id);
    void setTransport(Transport transport);

signals:
    void statusMessage(const QString& text);

private:
    void syncScrollBars();

    TimelineView* m_view = nullptr;
    QLabel* m_title = nullptr;
    QToolButton* m_selectTool = nullptr;
    QToolButton* m_razorTool = nullptr;
    QToolButton* m_snap = nullptr;
    QSlider* m_zoom = nullptr;
    QScrollBar* m_hScroll = nullptr;
    QScrollBar* m_vScroll = nullptr;
    QTimer* m_timer = nullptr;
    bool m_syncing = false;
};

} // namespace loki::ui::timeline
