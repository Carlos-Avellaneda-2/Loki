#include "app/main_window.h"

#include <DockManager.h>
#include <DockWidget.h>

#include <QSettings>
#include <QTemporaryDir>

#include <gtest/gtest.h>

namespace {

using loki::app::MainWindow;
using loki::ui::kAllPanels;
using loki::ui::PanelId;

QString objectNameOf(PanelId id) {
    const auto name = loki::ui::panelObjectName(id);
    return QString::fromUtf8(name.data(), static_cast<qsizetype>(name.size()));
}

TEST(MainWindowTest, CreatesEveryPanelAsDockWidget) {
    MainWindow window;
    for (const auto id : kAllPanels) {
        auto* dock = window.dockWidget(id);
        ASSERT_NE(dock, nullptr);
        EXPECT_EQ(dock->objectName(), objectNameOf(id));
        EXPECT_NE(dock->widget(), nullptr);
        EXPECT_EQ(window.dockManager()->findDockWidget(objectNameOf(id)), dock);
    }
    EXPECT_EQ(window.dockManager()->dockWidgetsMap().size(), static_cast<qsizetype>(kAllPanels.size()));
}

TEST(MainWindowTest, PanelsAreClosableMovableAndFloatable) {
    MainWindow window;
    for (const auto id : kAllPanels) {
        const auto features = window.dockWidget(id)->features();
        EXPECT_TRUE(features.testFlag(ads::CDockWidget::DockWidgetClosable));
        EXPECT_TRUE(features.testFlag(ads::CDockWidget::DockWidgetMovable));
        EXPECT_TRUE(features.testFlag(ads::CDockWidget::DockWidgetFloatable));
    }
}

TEST(MainWindowTest, DefaultLayoutShowsAllPanelsDocked) {
    MainWindow window;
    window.show();
    for (const auto id : kAllPanels) {
        auto* dock = window.dockWidget(id);
        EXPECT_FALSE(dock->isClosed());
        EXPECT_FALSE(dock->isFloating());
    }
}

TEST(MainWindowTest, LayoutRoundTripRestoresClosedPanel) {
    QByteArray saved;
    {
        MainWindow window;
        window.show();
        window.dockWidget(PanelId::AudioMeters)->toggleView(false);
        ASSERT_TRUE(window.dockWidget(PanelId::AudioMeters)->isClosed());
        saved = window.saveLayout();
    }
    ASSERT_FALSE(saved.isEmpty());

    MainWindow restored;
    restored.show();
    ASSERT_FALSE(restored.dockWidget(PanelId::AudioMeters)->isClosed());
    EXPECT_TRUE(restored.restoreLayout(saved));
    EXPECT_TRUE(restored.dockWidget(PanelId::AudioMeters)->isClosed());
    EXPECT_FALSE(restored.dockWidget(PanelId::Timeline)->isClosed());
}

TEST(MainWindowTest, RejectsEmptyOrCorruptLayout) {
    MainWindow window;
    EXPECT_FALSE(window.restoreLayout(QByteArray()));
    EXPECT_FALSE(window.restoreLayout(QByteArrayLiteral("not a layout")));
    for (const auto id : kAllPanels) {
        EXPECT_FALSE(window.dockWidget(id)->isClosed());
    }
}

TEST(MainWindowTest, SettingsRoundTripUsesGivenStore) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto path = dir.filePath(QStringLiteral("layout.ini"));
    {
        MainWindow window;
        window.show();
        window.dockWidget(PanelId::EffectControls)->toggleView(false);
        QSettings settings(path, QSettings::IniFormat);
        window.saveSettings(settings);
    }
    MainWindow restored;
    restored.show();
    const QSettings settings(path, QSettings::IniFormat);
    EXPECT_TRUE(restored.restoreSettings(settings));
    EXPECT_TRUE(restored.dockWidget(PanelId::EffectControls)->isClosed());
}

TEST(MainWindowTest, RestoreFromEmptySettingsKeepsDefaults) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QSettings empty(dir.filePath(QStringLiteral("empty.ini")), QSettings::IniFormat);
    MainWindow window;
    EXPECT_FALSE(window.restoreSettings(empty));
    for (const auto id : kAllPanels) {
        EXPECT_FALSE(window.dockWidget(id)->isClosed());
    }
}

} // namespace
