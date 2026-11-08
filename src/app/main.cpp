#include "app/main_window.h"
#include "app/qt_log_bridge.h"
#include "app/theme.h"
#include "util/logging.h"

#include <QApplication>
#include <QDir>
#include <QSettings>
#include <QStandardPaths>

#include <spdlog/spdlog.h>

#include <filesystem>

int main(int argc, char* argv[]) {
    QApplication::setOrganizationName(QStringLiteral("Loki"));
    QApplication::setApplicationName(QStringLiteral("Loki"));
    QApplication::setApplicationVersion(QStringLiteral(LOKI_VERSION));

    QApplication app(argc, argv);

    const auto dataDir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    loki::util::LogConfig logConfig;
    logConfig.directory = std::filesystem::path(QDir(dataDir).filePath(QStringLiteral("logs")).toStdWString());
#ifndef NDEBUG
    logConfig.level = spdlog::level::debug;
#endif
    const loki::util::LoggingGuard logging(logConfig);
    loki::app::installQtLogBridge();
    spdlog::info("Loki {} starting (Qt {})", LOKI_VERSION, qVersion());
    if (logging.result().fileLogging) {
        spdlog::info("Log file: {}", logging.result().logFilePath.string());
    }

    loki::app::applyDarkTheme(app);

    loki::app::MainWindow window;
    bool geometryRestored = false;
    {
        const QSettings settings;
        geometryRestored = window.restoreSettings(settings);
    }
    QObject::connect(&app, &QApplication::aboutToQuit, &window, [&window] {
        QSettings settings;
        window.saveSettings(settings);
    });
    if (geometryRestored) {
        window.show();
    } else {
        window.showMaximized(); // first run: fill the screen like Premiere does
    }

    const int exitCode = QApplication::exec();
    spdlog::info("Loki exiting with code {}", exitCode);
    return exitCode;
}
