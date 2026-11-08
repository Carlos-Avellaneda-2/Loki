#include "ui/placeholder_panel.h"

#include <QLabel>
#include <QVBoxLayout>

namespace loki::ui {

PlaceholderPanel::PlaceholderPanel(PanelId id, QWidget* parent) : QWidget(parent), m_id(id) {
    const auto title = panelTitle(id);
    // Qt parent-child ownership: the layout and label are owned by this widget.
    auto* layout = new QVBoxLayout(this);
    auto* label = new QLabel(QString::fromUtf8(title.data(), static_cast<qsizetype>(title.size())), this);
    label->setAlignment(Qt::AlignCenter);
    label->setObjectName(QStringLiteral("placeholderLabel"));
    layout->addWidget(label);
    setMinimumSize(120, 80);
}

} // namespace loki::ui
