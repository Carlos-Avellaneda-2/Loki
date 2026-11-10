#include "ui/speed_dialog.h"

#include "model/types.h"
#include "util/timecode.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QVBoxLayout>

#include <cmath>

namespace loki::ui {

namespace {

QString timecodeOf(const Rational& t, const Rational& rate) {
    const auto frames = timeToFrames(t, rate, Rounding::Nearest);
    return QString::fromStdString(formatTimecode(frames, rate, supportsDropFrame(rate)));
}

} // namespace

SpeedDialog::SpeedDialog(Rational span, Rational frameRate, double speedPercent, bool reverse, QWidget* parent)
    : QDialog(parent), m_span(span), m_frameRate(frameRate) {
    setWindowTitle(tr("Clip Speed / Duration"));
    auto* form = new QFormLayout;
    m_speed = new QDoubleSpinBox;
    m_speed->setRange(0.01, model::kMaxClipSpeed * 100.0);
    m_speed->setDecimals(2);
    m_speed->setSuffix(QStringLiteral(" %"));
    m_speed->setValue(speedPercent > 0.0 ? speedPercent : 100.0);
    form->addRow(tr("Speed:"), m_speed);
    m_duration = new QLineEdit;
    form->addRow(tr("Duration:"), m_duration);
    m_reverse = new QCheckBox(tr("Reverse Speed"));
    m_reverse->setChecked(reverse);
    m_ripple = new QCheckBox(tr("Ripple Edit, Shifting Trailing Clips"));
    m_error = new QLabel;
    m_error->setStyleSheet(QStringLiteral("color: #e06060"));
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(m_reverse);
    layout->addWidget(m_ripple);
    layout->addWidget(m_error);
    layout->addWidget(buttons);
    connect(m_speed, &QDoubleSpinBox::valueChanged, this, [this] { speedEdited(); });
    connect(m_duration, &QLineEdit::editingFinished, this, [this] { durationEdited(); });
    speedEdited();
}

double SpeedDialog::speedPercent() const {
    return m_speed->value();
}

bool SpeedDialog::reverse() const {
    return m_reverse->isChecked();
}

bool SpeedDialog::ripple() const {
    return m_ripple->isChecked();
}

Rational SpeedDialog::speed() const {
    return Rational(std::llround(m_speed->value() * 100.0), 10000);
}

void SpeedDialog::speedEdited() {
    if (m_updating) {
        return;
    }
    m_updating = true;
    m_duration->setText(timecodeOf(m_span / speed(), m_frameRate));
    m_error->clear();
    m_updating = false;
}

void SpeedDialog::durationEdited() {
    if (m_updating) {
        return;
    }
    const auto frames = parseTimecode(m_duration->text().toStdString(), m_frameRate, supportsDropFrame(m_frameRate));
    if (!frames || *frames <= 0 || !m_span.isPositive()) {
        m_error->setText(tr("Enter a duration like 00:00:05:00"));
        return;
    }
    const Rational duration = framesToTime(*frames, m_frameRate);
    m_updating = true;
    m_speed->setValue(std::clamp((m_span / duration).toDouble() * 100.0, m_speed->minimum(), m_speed->maximum()));
    m_error->clear();
    m_updating = false;
}

} // namespace loki::ui
