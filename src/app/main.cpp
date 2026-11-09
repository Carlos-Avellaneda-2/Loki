#include "app/main_window.h"
#include "app/qt_log_bridge.h"
#include "app/theme.h"
#include "ui/media_services.h"
#include "ui/project_controller.h"
#include "ui/source_monitor_panel.h"
#include "media/ffmpeg_utils.h"
#include "util/logging.h"

#include <QApplication>
#include <QDir>
#include <QSettings>
#include <QStandardPaths>
#include <QSurfaceFormat>

#include <spdlog/spdlog.h>

#include <filesystem>

int main(int argc, char* argv[]) {
    QApplication::setOrganizationName(QStringLiteral("Loki"));
    QApplication::setApplicationName(QStringLiteral("Loki"));
    QApplication::setApplicationVersion(QStringLiteral(LOKI_VERSION));

    // OpenGL 3.3 for the monitors (render module, native child windows: see ui/video_view.h).
    // Compatibility profile: "#version 330 core" shaders run fine in it and it is the most
    // widely supported context on Windows drivers.
    QSurfaceFormat format;
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CompatibilityProfile);
    format.setSwapInterval(1);
    QSurfaceFormat::setDefaultFormat(format);

    QApplication app(argc, argv);

    const auto dataDir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    loki::util::LogConfig logConfig;
    logConfig.directory = std::filesystem::path(QDir(dataDir).filePath(QStringLiteral("logs")).toStdWString());
#ifndef NDEBUG
    logConfig.level = spdlog::level::debug;
#endif
    const loki::util::LoggingGuard logging(logConfig);
    loki::app::installQtLogBridge();
    loki::media::installFfmpegLogBridge();
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

    // Command line: a .loki file opens that project; any other paths are imported as media.
    QStringList importPaths;
    for (const auto& arg : QApplication::arguments().mid(1)) {
        if (arg.endsWith(QStringLiteral(".loki"), Qt::CaseInsensitive)) {
            window.projectController()->openProject(arg);
        } else if (!arg.startsWith(QLatin1Char('-'))) {
            importPaths << arg;
        }
    }
    if (!importPaths.isEmpty()) {
        if (importPaths.size() == 1) {
            // Opening a single media file with Loki shows it in the Source Monitor once imported.
            const auto file = std::filesystem::path(importPaths.front().toStdWString()).lexically_normal();
            QObject::connect(window.mediaServices(), &loki::ui::MediaServices::importFinished, &window,
                             [&window, file](int, const QStringList&) {
                                 for (const auto& m : window.projectController()->project().media()) {
                                     if (std::error_code ec; !m.isGenerated() && std::filesystem::equivalent(m.path, file, ec)) {
                                         window.sourceMonitor()->loadMedia(m.id);
                                         break;
                                     }
                                 }
                             }, Qt::SingleShotConnection);
        }
        window.mediaServices()->importPaths(importPaths, {});
    }

    const int exitCode = QApplication::exec();
    spdlog::info("Loki exiting with code {}", exitCode);
    return exitCode;
}
