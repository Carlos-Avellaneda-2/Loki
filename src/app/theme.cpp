#include "app/theme.h"

#include <QApplication>
#include <QColor>
#include <QFile>
#include <QPalette>
#include <QStyleFactory>

#include <spdlog/spdlog.h>

namespace loki::app {

namespace {
constexpr auto kStyleSheetPath = ":/loki/theme/dark.qss";
} // namespace

QPalette darkPalette() {
    const QColor window(0x23, 0x23, 0x23);
    const QColor base(0x1b, 0x1b, 0x1b);
    const QColor alternateBase(0x28, 0x28, 0x28);
    const QColor text(0xcc, 0xcc, 0xcc);
    const QColor disabledText(0x6e, 0x6e, 0x6e);
    const QColor button(0x2e, 0x2e, 0x2e);
    const QColor highlight(0x2d, 0x8c, 0xeb);

    QPalette p;
    p.setColor(QPalette::Window, window);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, base);
    p.setColor(QPalette::AlternateBase, alternateBase);
    p.setColor(QPalette::ToolTipBase, button);
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::PlaceholderText, disabledText);
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, button);
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::BrightText, Qt::white);
    p.setColor(QPalette::Highlight, highlight);
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::Link, highlight);
    p.setColor(QPalette::Light, QColor(0x3a, 0x3a, 0x3a));
    p.setColor(QPalette::Midlight, QColor(0x32, 0x32, 0x32));
    p.setColor(QPalette::Mid, QColor(0x26, 0x26, 0x26));
    p.setColor(QPalette::Dark, QColor(0x14, 0x14, 0x14));
    p.setColor(QPalette::Shadow, Qt::black);

    for (const auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText}) {
        p.setColor(QPalette::Disabled, role, disabledText);
    }
    p.setColor(QPalette::Disabled, QPalette::Highlight, QColor(0x3a, 0x3a, 0x3a));
    return p;
}

void applyDarkTheme(QApplication& app) {
    if (auto* fusion = QStyleFactory::create(QStringLiteral("Fusion"))) {
        app.setStyle(fusion); // QApplication takes ownership
    }
    app.setPalette(darkPalette());

    QFile qss(QString::fromLatin1(kStyleSheetPath));
    if (qss.open(QIODevice::ReadOnly | QIODevice::Text)) {
        app.setStyleSheet(QString::fromUtf8(qss.readAll()));
    } else {
        spdlog::warn("Dark stylesheet not found at {}", kStyleSheetPath);
    }
}

} // namespace loki::app
