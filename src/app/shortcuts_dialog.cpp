#include "app/shortcuts_dialog.h"

#include <QDialogButtonBox>
#include <QHeaderView>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <set>

namespace loki::app {

ShortcutsDialog::ShortcutsDialog(ui::ShortcutManager& manager, QWidget* parent)
    : QDialog(parent), m_manager(manager), m_entries(manager.entries()) {
    setWindowTitle(tr("Keyboard Shortcuts"));
    resize(640, 560);
    std::sort(m_entries.begin(), m_entries.end(), [](const auto& a, const auto& b) {
        return a.context != b.context ? a.context < b.context : a.text < b.text;
    });

    m_table = new QTableWidget(static_cast<int>(m_entries.size()), 3, this);
    m_table->setHorizontalHeaderLabels({tr("Panel"), tr("Command"), tr("Shortcut")});
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->verticalHeader()->setVisible(false);
    m_table->setSelectionMode(QAbstractItemView::NoSelection);
    for (int row = 0; row < static_cast<int>(m_entries.size()); ++row) {
        const auto& e = m_entries[static_cast<std::size_t>(row)];
        auto* context = new QTableWidgetItem(e.context);
        context->setFlags(Qt::ItemIsEnabled);
        auto* text = new QTableWidgetItem(e.text);
        text->setFlags(Qt::ItemIsEnabled);
        text->setToolTip(tr("Default: %1").arg(e.defaultKey.toString(QKeySequence::NativeText)));
        m_table->setItem(row, 0, context);
        m_table->setItem(row, 1, text);
        auto* edit = new QKeySequenceEdit(e.key, m_table);
        edit->setClearButtonEnabled(true);
        connect(edit, &QKeySequenceEdit::keySequenceChanged, this,
                [this, row](const QKeySequence& key) { setPendingKey(row, key); });
        m_table->setCellWidget(row, 2, edit);
    }
    m_table->resizeColumnsToContents();

    m_status = new QLabel(this);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    auto* defaults = buttons->addButton(tr("Restore Defaults"), QDialogButtonBox::ResetRole);
    connect(defaults, &QPushButton::clicked, this, [this] {
        for (int row = 0; row < static_cast<int>(m_entries.size()); ++row) {
            auto& e = m_entries[static_cast<std::size_t>(row)];
            e.key = e.defaultKey;
            if (auto* edit = qobject_cast<QKeySequenceEdit*>(m_table->cellWidget(row, 2))) {
                edit->blockSignals(true);
                edit->setKeySequence(e.key);
                edit->blockSignals(false);
            }
        }
        refreshConflicts();
    });
    connect(buttons, &QDialogButtonBox::accepted, this, &ShortcutsDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(m_table, 1);
    layout->addWidget(m_status);
    layout->addWidget(buttons);
    refreshConflicts();
}

void ShortcutsDialog::setPendingKey(int row, const QKeySequence& key) {
    if (row < 0 || row >= static_cast<int>(m_entries.size())) {
        return;
    }
    // Only the first chord counts (Premiere-style single-key shortcuts).
    m_entries[static_cast<std::size_t>(row)].key = key.isEmpty() ? QKeySequence() : QKeySequence(key[0]);
    refreshConflicts();
}

bool ShortcutsDialog::hasConflicts() const {
    return !ui::ShortcutManager::conflicts(m_entries).empty();
}

void ShortcutsDialog::refreshConflicts() {
    const auto conflicts = ui::ShortcutManager::conflicts(m_entries);
    std::set<QString> bad;
    for (const auto& [a, b] : conflicts) {
        bad.insert(a);
        bad.insert(b);
    }
    for (int row = 0; row < static_cast<int>(m_entries.size()); ++row) {
        const bool conflict = bad.contains(m_entries[static_cast<std::size_t>(row)].id);
        for (int col = 0; col < 2; ++col) {
            if (auto* item = m_table->item(row, col)) {
                item->setForeground(conflict ? QColor(0xff, 0x70, 0x70) : palette().color(QPalette::Text));
            }
        }
    }
    m_status->setText(conflicts.empty() ? QString()
                                        : tr("%n conflicting shortcut(s) - fix them before applying.", nullptr,
                                             static_cast<int>(conflicts.size())));
}

void ShortcutsDialog::accept() {
    if (hasConflicts()) {
        refreshConflicts();
        return;
    }
    for (const auto& e : m_entries) {
        if (e.key != m_manager.shortcut(e.id)) {
            m_manager.setShortcut(e.id, e.key);
        }
    }
    QDialog::accept();
}

} // namespace loki::app
