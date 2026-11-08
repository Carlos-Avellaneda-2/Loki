#include "app/main_window.h"

#include "ui/panel_factory.h"

#include <DockAreaWidget.h>
#include <DockManager.h>
#include <DockWidget.h>

#include <QAction>
#include <QApplication>
#include <QKeySequence>
#include <QMenu>
#include <QMenuBar>
#include <QSettings>

#include <spdlog/spdlog.h>

namespace loki::app {

namespace {

constexpr auto kGeometryKey = "mainWindow/geometry";
constexpr auto kDockStateKey = "mainWindow/dockState";

QString toQString(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

constexpr std::size_t indexOf(ui::PanelId id) noexcept {
    return static_cast<std::size_t>(id);
}

} // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("Loki"));
    resize(1280, 720);

    // ADS config flags are process-wide and must be set before the dock manager is created.
    ads::CDockManager::setConfigFlag(ads::CDockManager::OpaqueSplitterResize, true);
    ads::CDockManager::setConfigFlag(ads::CDockManager::FocusHighlighting, true);
    ads::CDockManager::setConfigFlag(ads::CDockManager::AllTabsHaveCloseButton, false);

    m_dockManager = new ads::CDockManager(this);
    // Drop ADS's built-in light stylesheet; the application dark theme styles dock widgets.
    m_dockManager->setStyleSheet(QString());

    createDockWidgets();
    createMenus();
}

MainWindow::~MainWindow() = default;

ads::CDockWidget* MainWindow::dockWidget(ui::PanelId id) const noexcept {
    return m_docks[indexOf(id)];
}

void MainWindow::createDockWidgets() {
    for (const auto id : ui::kAllPanels) {
        auto* dock = new ads::CDockWidget(m_dockManager, toQString(ui::panelTitle(id)));
        dock->setObjectName(toQString(ui::panelObjectName(id)));
        dock->setWidget(ui::createPanel(id), ads::CDockWidget::ForceNoScrollArea);
        dock->setFeature(ads::CDockWidget::DockWidgetClosable, true);
        dock->setFeature(ads::CDockWidget::DockWidgetMovable, true);
        dock->setFeature(ads::CDockWidget::DockWidgetFloatable, true);
        m_docks[indexOf(id)] = dock;
    }

    using ui::PanelId;
    // Default "Editing" workspace, Premiere-like:
    //   [ Source | Effect Controls (tabs) ] [ Program            ]
    //   [ Project ] [ Timeline                    ] [ Audio Meters ]
    auto* sourceArea = m_dockManager->addDockWidget(ads::TopDockWidgetArea, dockWidget(PanelId::SourceMonitor));
    m_dockManager->addDockWidgetTabToArea(dockWidget(PanelId::EffectControls), sourceArea);
    m_dockManager->addDockWidget(ads::RightDockWidgetArea, dockWidget(PanelId::ProgramMonitor), sourceArea);

    auto* timelineArea = m_dockManager->addDockWidget(ads::BottomDockWidgetArea, dockWidget(PanelId::Timeline));
    m_dockManager->addDockWidget(ads::LeftDockWidgetArea, dockWidget(PanelId::Project), timelineArea);
    m_dockManager->addDockWidget(ads::RightDockWidgetArea, dockWidget(PanelId::AudioMeters), timelineArea);

    sourceArea->setCurrentDockWidget(dockWidget(PanelId::SourceMonitor));
    // Relative widths of the bottom row (Project | Timeline | Audio Meters); QSplitter rescales them.
    m_dockManager->setSplitterSizes(timelineArea, {300, 900, 150});
}

void MainWindow::createMenus() {
    auto* fileMenu = menuBar()->addMenu(tr("&File"));
    auto* quitAction = fileMenu->addAction(tr("E&xit"));
    quitAction->setShortcut(QKeySequence::Quit);
    quitAction->setMenuRole(QAction::QuitRole);
    connect(quitAction, &QAction::triggered, this, &QWidget::close);

    auto* windowMenu = menuBar()->addMenu(tr("&Window"));
    for (const auto id : ui::kAllPanels) {
        windowMenu->addAction(dockWidget(id)->toggleViewAction());
    }
}

QByteArray MainWindow::saveLayout() const {
    return m_dockManager->saveState(kLayoutVersion);
}

bool MainWindow::restoreLayout(const QByteArray& state) {
    if (state.isEmpty()) {
        return false;
    }
    const bool ok = m_dockManager->restoreState(state, kLayoutVersion);
    if (!ok) {
        spdlog::warn("Saved dock layout rejected (incompatible version or corrupt); using default layout");
    }
    return ok;
}

void MainWindow::saveSettings(QSettings& settings) const {
    settings.setValue(kGeometryKey, saveGeometry());
    settings.setValue(kDockStateKey, saveLayout());
}

bool MainWindow::restoreSettings(const QSettings& settings) {
    const auto geometry = settings.value(kGeometryKey).toByteArray();
    const bool geometryRestored = !geometry.isEmpty() && restoreGeometry(geometry);
    restoreLayout(settings.value(kDockStateKey).toByteArray());
    return geometryRestored;
}

} // namespace loki::app
