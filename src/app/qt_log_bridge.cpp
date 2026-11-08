#include "app/qt_log_bridge.h"

#include <QtGlobal>
#include <QString>

#include <spdlog/spdlog.h>

namespace loki::app {

namespace {

void qtMessageHandler(QtMsgType type, const QMessageLogContext& context, const QString& message) {
    const auto text = message.toStdString();
    const char* category = context.category != nullptr ? context.category : "qt";
    switch (type) {
    case QtDebugMsg: spdlog::debug("[{}] {}", category, text); break;
    case QtInfoMsg: spdlog::info("[{}] {}", category, text); break;
    case QtWarningMsg: spdlog::warn("[{}] {}", category, text); break;
    case QtCriticalMsg: spdlog::error("[{}] {}", category, text); break;
    case QtFatalMsg:
        spdlog::critical("[{}] {}", category, text);
        spdlog::default_logger()->flush();
        break; // Qt aborts after the handler returns
    }
}

} // namespace

void installQtLogBridge() {
    qInstallMessageHandler(&qtMessageHandler);
}

} // namespace loki::app
