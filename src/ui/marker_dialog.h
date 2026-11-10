#pragma once

#include "model/types.h"

#include <QColor>
#include <QDialog>

class QComboBox;
class QLineEdit;
class QPlainTextEdit;

namespace loki::ui {

// Display color of a marker (Premiere's palette).
[[nodiscard]] QColor markerQColor(model::MarkerColor color);

// Edits one marker (D29): name, comment, color, duration. "Delete" closes with deleteRequested().
class MarkerDialog final : public QDialog {
    Q_OBJECT

public:
    MarkerDialog(model::Marker marker, Rational frameRate, QWidget* parent = nullptr);

    // The edited marker (valid after accept()).
    [[nodiscard]] model::Marker marker() const;
    [[nodiscard]] bool deleteRequested() const noexcept { return m_delete; }

private:
    model::Marker m_marker;
    Rational m_frameRate;
    QLineEdit* m_name = nullptr;
    QPlainTextEdit* m_comment = nullptr;
    QComboBox* m_color = nullptr;
    QLineEdit* m_duration = nullptr;
    bool m_delete = false;
};

} // namespace loki::ui
