#pragma once

#include "ui/shortcut_manager.h"

#include <QDialog>

#include <vector>

class QLabel;
class QTableWidget;

namespace loki::app {

// Edit > Keyboard Shortcuts: every registered command with its context and key. Conflicting keys
// (same key in the same panel, or clashing with an application-wide one) are highlighted and must
// be fixed before applying.
class ShortcutsDialog final : public QDialog {
    Q_OBJECT

public:
    explicit ShortcutsDialog(ui::ShortcutManager& manager, QWidget* parent = nullptr);

    // Pending (not yet applied) entries; public for tests.
    [[nodiscard]] const std::vector<ui::ShortcutManager::Entry>& pending() const noexcept { return m_entries; }
    void setPendingKey(int row, const QKeySequence& key);
    [[nodiscard]] bool hasConflicts() const;

private:
    void accept() override;
    void refreshConflicts();

    ui::ShortcutManager& m_manager;
    std::vector<ui::ShortcutManager::Entry> m_entries;
    QTableWidget* m_table = nullptr;
    QLabel* m_status = nullptr;
};

} // namespace loki::app
