#pragma once

#include "ui/timeline/timeline_view.h"

#include <QWidget>

#include <utility>
#include <vector>

class QComboBox;
class QLabel;
class QScrollBar;
class QSlider;
class QTimer;
class QToolButton;

namespace loki::ui {
class ShortcutManager;
} // namespace loki::ui

namespace loki::ui::timeline {

// Timeline panel: tool bar (Selection/Razor, snapping, zoom), the timeline view and scroll bars.
class TimelinePanel final : public QWidget {
    Q_OBJECT

public:
    TimelinePanel(ProjectController& controller, MediaServices& services, QWidget* parent = nullptr);

    [[nodiscard]] TimelineView* view() const noexcept { return m_view; }
    void setSequence(model::SequenceId id);
    void setTransport(Transport transport);
    // Creates the timeline's keyboard actions (Premiere defaults, user-configurable).
    void registerShortcuts(ShortcutManager& shortcuts);

    // Sequence switcher (every sequence of the project; the active one selected).
    [[nodiscard]] QComboBox* sequenceSelector() const noexcept { return m_sequences; }

signals:
    void statusMessage(const QString& text);
    void sequenceRequested(loki::model::SequenceId id);

private:
    void syncScrollBars();

    TimelineView* m_view = nullptr;
    void refreshSequences();

    ProjectController& m_controller;
    QComboBox* m_sequences = nullptr;
    bool m_refreshing = false;
    std::vector<std::pair<Tool, QToolButton*>> m_toolButtons;
    QToolButton* m_snap = nullptr;
    QSlider* m_zoom = nullptr;
    QScrollBar* m_hScroll = nullptr;
    QScrollBar* m_vScroll = nullptr;
    QTimer* m_timer = nullptr;
    bool m_syncing = false;
};

} // namespace loki::ui::timeline
