#include "app/main_window.h"
#include "app/shortcuts_dialog.h"
#include "ui/shortcut_manager.h"

#include <QAction>
#include <QSettings>
#include <QWidget>

#include <gtest/gtest.h>

namespace {

using loki::ui::ShortcutManager;

class ShortcutsTest : public ::testing::Test {
protected:
    void SetUp() override { QSettings().remove(QStringLiteral("shortcuts")); }
    void TearDown() override { QSettings().remove(QStringLiteral("shortcuts")); }
};

TEST_F(ShortcutsTest, DefaultsApplyAndChangesPersist) {
    QWidget owner;
    {
        ShortcutManager manager;
        int fired = 0;
        auto* action = manager.create(&owner, QStringLiteral("test.a"), QStringLiteral("Panel"), QStringLiteral("A"),
                                      QKeySequence(Qt::Key_A), [&] { ++fired; });
        EXPECT_EQ(action->shortcut(), QKeySequence(Qt::Key_A));
        action->trigger();
        EXPECT_EQ(fired, 1);
        manager.setShortcut(QStringLiteral("test.a"), QKeySequence(Qt::CTRL | Qt::Key_B));
        EXPECT_EQ(action->shortcut(), QKeySequence(Qt::CTRL | Qt::Key_B));
    }
    // A new manager (next app start) loads the user's key.
    ShortcutManager again;
    auto* action = again.create(&owner, QStringLiteral("test.a"), QStringLiteral("Panel"), QStringLiteral("A"),
                                QKeySequence(Qt::Key_A), [] {});
    EXPECT_EQ(action->shortcut(), QKeySequence(Qt::CTRL | Qt::Key_B));
    again.resetAll();
    EXPECT_EQ(action->shortcut(), QKeySequence(Qt::Key_A));
}

TEST_F(ShortcutsTest, ConflictsAreScopedToContexts) {
    std::vector<ShortcutManager::Entry> entries = {
        {QStringLiteral("t.a"), QStringLiteral("Timeline"), QStringLiteral("A"), {}, QKeySequence(Qt::Key_I)},
        {QStringLiteral("s.a"), QStringLiteral("Source"), QStringLiteral("B"), {}, QKeySequence(Qt::Key_I)},
    };
    EXPECT_TRUE(ShortcutManager::conflicts(entries).empty()); // same key, different panels: fine
    entries.push_back({QStringLiteral("t.b"), QStringLiteral("Timeline"), QStringLiteral("C"), {}, QKeySequence(Qt::Key_I)});
    EXPECT_EQ(ShortcutManager::conflicts(entries).size(), 1u);
    entries.push_back({QStringLiteral("app.x"), QString::fromLatin1(ShortcutManager::kApplicationContext),
                       QStringLiteral("D"), {}, QKeySequence(Qt::Key_I)});
    EXPECT_EQ(ShortcutManager::conflicts(entries).size(), 4u); // app-wide clashes with everything
}

TEST_F(ShortcutsTest, MainWindowRegistersPremiereDefaultsWithoutConflicts) {
    loki::app::MainWindow window;
    auto* manager = window.shortcuts();
    EXPECT_EQ(manager->shortcut(QStringLiteral("timeline.tool.ripple")), QKeySequence(Qt::Key_B));
    EXPECT_EQ(manager->shortcut(QStringLiteral("timeline.rippleDelete")), QKeySequence(Qt::SHIFT | Qt::Key_Delete));
    EXPECT_EQ(manager->shortcut(QStringLiteral("source.insert")), QKeySequence(Qt::Key_Comma));
    EXPECT_EQ(manager->shortcut(QStringLiteral("program.lift")), QKeySequence(Qt::Key_Semicolon));
    EXPECT_EQ(manager->shortcut(QStringLiteral("app.undo")), QKeySequence(Qt::CTRL | Qt::Key_Z));
    EXPECT_GT(manager->entries().size(), 60u);
    EXPECT_TRUE(ShortcutManager::conflicts(manager->entries()).empty());
}

TEST_F(ShortcutsTest, DialogBlocksConflictsAndApplies) {
    loki::app::MainWindow window;
    loki::app::ShortcutsDialog dialog(*window.shortcuts());
    const auto& pending = dialog.pending();
    int ripple = -1;
    int razor = -1;
    for (int i = 0; i < static_cast<int>(pending.size()); ++i) {
        if (pending[static_cast<std::size_t>(i)].id == QStringLiteral("timeline.tool.ripple")) {
            ripple = i;
        }
        if (pending[static_cast<std::size_t>(i)].id == QStringLiteral("timeline.tool.razor")) {
            razor = i;
        }
    }
    ASSERT_GE(ripple, 0);
    ASSERT_GE(razor, 0);
    dialog.setPendingKey(ripple, QKeySequence(Qt::Key_C)); // clashes with Razor
    EXPECT_TRUE(dialog.hasConflicts());
    dialog.setPendingKey(razor, QKeySequence(Qt::Key_X));
    EXPECT_FALSE(dialog.hasConflicts());
    static_cast<QDialog&>(dialog).accept();
    EXPECT_EQ(window.shortcuts()->shortcut(QStringLiteral("timeline.tool.ripple")), QKeySequence(Qt::Key_C));
}

} // namespace
