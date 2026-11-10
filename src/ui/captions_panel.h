#pragma once

#include "model/types.h"

#include <QWidget>

#include <functional>

class QCheckBox;
class QDoubleSpinBox;
class QFontComboBox;
class QPlainTextEdit;
class QToolButton;
class QTreeWidget;

namespace loki::ui {

class ProjectController;

// Captions (D40) of the active sequence: list (start, end, text), add at the playhead, edit the
// selected one, delete, import / export SubRip (.srt), and the sequence's caption style.
class CaptionsPanel final : public QWidget {
    Q_OBJECT

public:
    explicit CaptionsPanel(ProjectController& controller, QWidget* parent = nullptr);

    void setSequence(model::SequenceId id);
    void setPlayhead(std::function<Rational()> position) { m_position = std::move(position); }
    [[nodiscard]] int captionCount() const;

    // Actions (public for tests and menus).
    void addAtPlayhead();
    void deleteSelected();
    bool importSrt(const QString& file);
    bool exportSrt(const QString& file);
    void selectCaption(model::CaptionId id);
    // Applies the editor text to the selected caption.
    void applyText(const QString& text);

signals:
    void seekRequested(loki::Rational time);

private:
    void rebuild();
    void refreshStyle();
    void applyStyle();
    [[nodiscard]] model::CaptionId selectedCaption() const;
    [[nodiscard]] const model::Sequence* sequence() const;

    ProjectController& m_controller;
    model::SequenceId m_sequence;
    std::function<Rational()> m_position;
    QTreeWidget* m_list = nullptr;
    QPlainTextEdit* m_text = nullptr;
    QFontComboBox* m_font = nullptr;
    QDoubleSpinBox* m_size = nullptr;
    QToolButton* m_color = nullptr;
    QDoubleSpinBox* m_background = nullptr;
    QDoubleSpinBox* m_position_y = nullptr;
    QCheckBox* m_visible = nullptr;
    bool m_updating = false;
};

} // namespace loki::ui
