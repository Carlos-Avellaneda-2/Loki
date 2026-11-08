#pragma once

#include "ui/panel_id.h"

class QWidget;

namespace loki::ui {

// Creates the content widget for a panel. The caller (or the given parent) owns the result.
[[nodiscard]] QWidget* createPanel(PanelId id, QWidget* parent = nullptr);

} // namespace loki::ui
