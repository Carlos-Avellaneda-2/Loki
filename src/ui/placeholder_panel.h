#pragma once

#include "ui/panel_id.h"

#include <QWidget>

namespace loki::ui {

// Empty panel shown until the real implementation of a panel lands in its phase.
class PlaceholderPanel final : public QWidget {
    Q_OBJECT

public:
    explicit PlaceholderPanel(PanelId id, QWidget* parent = nullptr);

    [[nodiscard]] PanelId panelId() const noexcept { return m_id; }

private:
    PanelId m_id;
};

} // namespace loki::ui
