#pragma once

#include "model/types.h"

#include <QWidget>

#include <functional>

class QTreeWidget;

namespace loki::ui {

class ProjectController;

// Markers panel (D29): the sequence markers of the active sequence with their time, duration,
// name, color and comment. Double-click jumps there; Edit/Delete act on the selected marker.
class MarkersPanel final : public QWidget {
    Q_OBJECT

public:
    explicit MarkersPanel(ProjectController& controller, QWidget* parent = nullptr);

    void setSequence(model::SequenceId id);
    [[nodiscard]] int markerCount() const;
    // Opens the marker editor (set by the main window: the Timeline's dialog).
    void setEditor(std::function<void(model::MarkerId)> editor) { m_editor = std::move(editor); }

signals:
    void seekRequested(loki::Rational time);

private:
    void rebuild();
    [[nodiscard]] model::MarkerId selectedMarker() const;

    ProjectController& m_controller;
    model::SequenceId m_sequence;
    QTreeWidget* m_list = nullptr;
    std::function<void(model::MarkerId)> m_editor;
};

} // namespace loki::ui
