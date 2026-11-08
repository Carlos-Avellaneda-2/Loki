#include "ui/panel_factory.h"

#include "ui/placeholder_panel.h"

namespace loki::ui {

QWidget* createPanel(PanelId id, QWidget* parent) {
    // Phase 1: every panel is a placeholder. Real panels replace these cases phase by phase.
    return new PlaceholderPanel(id, parent);
}

} // namespace loki::ui
