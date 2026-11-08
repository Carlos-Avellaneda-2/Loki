#include "app/theme.h"

#include <QApplication>
#include <QPalette>
#include <QStyleFactory>

#include <gtest/gtest.h>

namespace {

TEST(ThemeTest, DarkPaletteHasDarkBackgroundAndLightText) {
    const auto palette = loki::app::darkPalette();
    EXPECT_LT(palette.color(QPalette::Window).lightness(), 64);
    EXPECT_LT(palette.color(QPalette::Base).lightness(), 64);
    EXPECT_GT(palette.color(QPalette::WindowText).lightness(), 160);
    EXPECT_GT(palette.color(QPalette::Text).lightness(), 160);
}

TEST(ThemeTest, DisabledTextIsDimmerThanActiveText) {
    const auto palette = loki::app::darkPalette();
    EXPECT_LT(palette.color(QPalette::Disabled, QPalette::Text).lightness(),
              palette.color(QPalette::Active, QPalette::Text).lightness());
}

TEST(ThemeTest, ApplyDarkThemeSetsPaletteAndStyleSheet) {
    auto* app = qobject_cast<QApplication*>(QCoreApplication::instance());
    ASSERT_NE(app, nullptr);
    // Fusion is the base style; once a stylesheet is set QApplication::style() is Qt's internal
    // stylesheet proxy, so the base style itself is not observable through public API.
    ASSERT_TRUE(QStyleFactory::keys().contains(QStringLiteral("Fusion"), Qt::CaseInsensitive));
    loki::app::applyDarkTheme(*app);
    EXPECT_FALSE(app->styleSheet().isEmpty());
    EXPECT_EQ(QApplication::palette().color(QPalette::Window), loki::app::darkPalette().color(QPalette::Window));
}

} // namespace
