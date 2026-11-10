#include "ui/marker_dialog.h"

#include "commands/marker_commands.h"
#include "util/timecode.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace loki::ui {

QColor markerQColor(model::MarkerColor color) {
    switch (color) {
    case model::MarkerColor::Green: return {0x5c, 0xc8, 0x5c};
    case model::MarkerColor::Red: return {0xe0, 0x4c, 0x4c};
    case model::MarkerColor::Purple: return {0xb0, 0x6c, 0xe0};
    case model::MarkerColor::Orange: return {0xf0, 0x98, 0x38};
    case model::MarkerColor::Yellow: return {0xf0, 0xd8, 0x40};
    case model::MarkerColor::White: return {0xf0, 0xf0, 0xf0};
    case model::MarkerColor::Blue: return {0x48, 0x8c, 0xf0};
    case model::MarkerColor::Cyan: return {0x48, 0xd8, 0xe0};
    }
    return {0x5c, 0xc8, 0x5c};
}

MarkerDialog::MarkerDialog(model::Marker marker, Rational frameRate, QWidget* parent)
    : QDialog(parent), m_marker(std::move(marker)), m_frameRate(frameRate) {
    setWindowTitle(tr("Marker"));
    const bool drop = supportsDropFrame(m_frameRate);
    auto* form = new QFormLayout;
    m_name = new QLineEdit(QString::fromStdString(m_marker.name));
    form->addRow(tr("Name:"), m_name);
    form->addRow(tr("In:"), new QLineEdit(QString::fromStdString(formatTimecode(
                                  timeToFrames(m_marker.time, m_frameRate, Rounding::Nearest), m_frameRate, drop))));
    m_duration = new QLineEdit(QString::fromStdString(
        formatTimecode(timeToFrames(m_marker.duration, m_frameRate, Rounding::Nearest), m_frameRate, drop)));
    form->addRow(tr("Duration:"), m_duration);
    m_color = new QComboBox;
    for (int i = 0; i < model::kMarkerColorCount; ++i) {
        const auto color = static_cast<model::MarkerColor>(i);
        QPixmap swatch(12, 12);
        swatch.fill(markerQColor(color));
        m_color->addItem(QIcon(swatch), tr(commands::markerColorName(color)));
    }
    m_color->setCurrentIndex(static_cast<int>(m_marker.color));
    form->addRow(tr("Color:"), m_color);
    m_comment = new QPlainTextEdit(QString::fromStdString(m_marker.comment));
    m_comment->setMaximumHeight(90);
    form->addRow(tr("Comments:"), m_comment);
    // The In field is informative (markers move by dragging or re-adding); keep it read-only.
    if (auto* in = qobject_cast<QLineEdit*>(form->itemAt(1, QFormLayout::FieldRole)->widget())) {
        in->setReadOnly(true);
    }

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    auto* remove = buttons->addButton(tr("Delete"), QDialogButtonBox::DestructiveRole);
    connect(remove, &QPushButton::clicked, this, [this] {
        m_delete = true;
        accept();
    });
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(buttons);
}

model::Marker MarkerDialog::marker() const {
    model::Marker m = m_marker;
    m.name = m_name->text().toStdString();
    m.comment = m_comment->toPlainText().toStdString();
    m.color = static_cast<model::MarkerColor>(std::max(0, m_color->currentIndex()));
    if (const auto frames = parseTimecode(m_duration->text().toStdString(), m_frameRate, supportsDropFrame(m_frameRate));
        frames && *frames >= 0) {
        m.duration = framesToTime(*frames, m_frameRate);
    }
    return m;
}

} // namespace loki::ui
