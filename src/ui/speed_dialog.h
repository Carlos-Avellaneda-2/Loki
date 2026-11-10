#pragma once

#include "util/rational.h"

#include <QDialog>

class QCheckBox;
class QDoubleSpinBox;
class QLineEdit;
class QLabel;

namespace loki::ui {

// Clip Speed/Duration (Ctrl+R, D28). Speed and duration are linked: the source content stays, so
// duration = source span / speed. Reverse plays the content backwards; Ripple Edit moves the
// clips after it instead of stopping at the next clip.
class SpeedDialog final : public QDialog {
    Q_OBJECT

public:
    // `span`: source time the clip covers; `frameRate`: sequence rate (timecode).
    SpeedDialog(Rational span, Rational frameRate, double speedPercent, bool reverse, QWidget* parent = nullptr);

    [[nodiscard]] double speedPercent() const;
    [[nodiscard]] bool reverse() const;
    [[nodiscard]] bool ripple() const;
    // Speed as an exact ratio (1 = 100 %), rounded to 1/100 %.
    [[nodiscard]] Rational speed() const;

private:
    void speedEdited();
    void durationEdited();

    Rational m_span;
    Rational m_frameRate;
    QDoubleSpinBox* m_speed = nullptr;
    QLineEdit* m_duration = nullptr;
    QLabel* m_error = nullptr;
    QCheckBox* m_reverse = nullptr;
    QCheckBox* m_ripple = nullptr;
    bool m_updating = false;
};

} // namespace loki::ui
