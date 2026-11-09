#include "app/main_window.h"

#include "model/project_json.h"
#include "ui/panel_factory.h"
#include "app/preferences_dialog.h"
#include "app/shortcuts_dialog.h"
#include "ui/shortcut_manager.h"
#include "ui/media_services.h"
#include "ui/project_controller.h"
#include "ui/project_panel.h"
#include "commands/edit_commands.h"
#include "ui/audio_meters.h"
#include "ui/audio_mixer_panel.h"
#include "ui/effect_controls_panel.h"
#include "ui/program_monitor_panel.h"
#include "ui/source_monitor_panel.h"
#include "ui/timeline/timeline_panel.h"
#include "util/timecode.h"

#include <DockAreaWidget.h>
#include <DockManager.h>
#include <DockWidget.h>

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QFileDialog>
#include <QKeySequence>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QSettings>
#include <QStatusBar>

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

MainWindow::MainWindow(QWidget* parent, playback::PlaybackOptions playbackOptions)
    : QMainWindow(parent), m_playbackOptions(std::move(playbackOptions)) {
    resize(1280, 720);
    m_controller = new ui::ProjectController(this);
    m_services = new ui::MediaServices(*m_controller, ui::configuredCacheDirectory(), this);
    m_shortcuts = new ui::ShortcutManager(this);
    connect(m_controller, &ui::ProjectController::modifiedChanged, this, &MainWindow::updateTitle);
    connect(m_controller, &ui::ProjectController::filePathChanged, this, &MainWindow::updateTitle);
    connect(m_controller, &ui::ProjectController::commandFailed, this,
            [this](const QString& message) { statusBar()->showMessage(message, 5000); });
    connect(m_controller, &ui::ProjectController::openFinished, this, [this](bool ok, const QString& error) {
        if (!ok) {
            QMessageBox::warning(this, tr("Open Project"), tr("Could not open the project.\n\n%1").arg(error));
        }
    });
    connect(m_controller, &ui::ProjectController::saveFinished, this, [this](bool ok, const QString& error) {
        if (ok) {
            statusBar()->showMessage(tr("Project saved"), 3000);
        } else {
            QMessageBox::warning(this, tr("Save Project"), tr("Could not save the project.\n\n%1").arg(error));
        }
    });
    updateTitle();

    // ADS config flags are process-wide and must be set before the dock manager is created.
    ads::CDockManager::setConfigFlag(ads::CDockManager::OpaqueSplitterResize, true);
    ads::CDockManager::setConfigFlag(ads::CDockManager::FocusHighlighting, true);
    ads::CDockManager::setConfigFlag(ads::CDockManager::AllTabsHaveCloseButton, false);

    m_dockManager = new ads::CDockManager(this);
    // Drop ADS's built-in light stylesheet; the application dark theme styles dock widgets.
    m_dockManager->setStyleSheet(QString());

    createDockWidgets();
    createMenus();
    connect(m_projectPanel, &ui::ProjectPanel::mediaActivated, this, [this](model::MediaId id) {
        m_sourceMonitor->loadMedia(id);
        dockWidget(ui::PanelId::SourceMonitor)->raise();
    });
    m_timeline->setTransport(m_programMonitor->transport());
    // Meters: whatever is playing (Program first, then Source); the mixer follows the sequence.
    m_audioMeters->addSource([this] { return m_programMonitor->engine().levels(); });
    m_audioMeters->addSource([this] { return m_sourceMonitor->engine().levels(); });
    m_audioMixer->setLevelSource([this] { return m_programMonitor->engine().levels(); });
    m_timeline->registerShortcuts(*m_shortcuts);
    m_sourceMonitor->registerShortcuts(*m_shortcuts);
    m_programMonitor->registerShortcuts(*m_shortcuts);
    m_effectControls->setPlayhead({[this] { return m_programMonitor->engine().position(); },
                                   [this](const Rational& t) { m_programMonitor->engine().seek(t); }});
    connect(m_timeline->view(), &ui::timeline::TimelineView::selectionChanged, this, [this] {
        // Show the selected clip (for a linked A/V pair, the video part).
        const auto& selection = m_timeline->view()->selection();
        const auto& project = m_controller->project();
        model::ClipId pick;
        model::LinkId link;
        bool single = true;
        for (const auto id : selection) {
            const auto ref = project.findClip(id);
            if (ref.clip == nullptr) {
                continue;
            }
            if (pick && (ref.clip->link.isNull() || ref.clip->link != link)) {
                single = false;
                break;
            }
            if (!pick || ref.track->kind == model::TrackKind::Video) {
                pick = id;
                link = ref.clip->link;
            }
        }
        m_effectControls->setClip(single ? pick : model::ClipId());
        if (single && pick) {
            dockWidget(ui::PanelId::EffectControls)->raise();
        }
    });
    connect(m_timeline, &ui::timeline::TimelinePanel::statusMessage, this,
            [this](const QString& text) { statusBar()->showMessage(text, 5000); });
    connect(m_sourceMonitor, &ui::SourceMonitorPanel::editRequested, this, &MainWindow::sourceEdit);
    connect(m_controller, &ui::ProjectController::projectReset, this, &MainWindow::activateFirstSequence);
    connect(m_controller, &ui::ProjectController::projectChanged, this, [this](const model::ChangeEvent& e) {
        if (e.kind == model::ChangeKind::SequenceRemoved && e.id == m_activeSequence.uuid()) {
            activateFirstSequence();
        } else if (e.kind == model::ChangeKind::SequenceAdded && !m_activeSequence) {
            activateFirstSequence();
        }
    });
    activateFirstSequence();
}

void MainWindow::activateFirstSequence() {
    const auto& sequences = m_controller->project().sequences();
    m_activeSequence = sequences.empty() ? model::SequenceId() : sequences.front().id;
    m_timeline->setSequence(m_activeSequence);
    m_programMonitor->setSequence(m_activeSequence);
    m_audioMixer->setSequence(m_activeSequence);
}

void MainWindow::sourceEdit(model::MediaId media, bool insert) {
    const auto& project = m_controller->project();
    const auto* item = project.findMedia(media);
    const auto* sequence = project.findSequence(m_activeSequence);
    if (item == nullptr || sequence == nullptr) {
        statusBar()->showMessage(tr("No active sequence"), 4000);
        return;
    }
    const auto range = commands::sourceRangeOf(*item);
    const Rational at = quantizeToFrame(m_programMonitor->engine().position(), sequence->settings.frameRate,
                                        Rounding::Floor);
    try {
        auto command = insert ? commands::makeInsertEdit(project, m_activeSequence, range, at,
                                                         m_timeline->view()->targets())
                              : commands::makeOverwriteEdit(project, m_activeSequence, range, at,
                                                            m_timeline->view()->targets());
        if (command && m_controller->execute(std::move(command))) {
            // Like Premiere: the playhead moves to the end of the new clip.
            m_programMonitor->engine().seek(at + range.duration);
        }
    } catch (const std::exception& e) {
        statusBar()->showMessage(QString::fromUtf8(e.what()), 5000);
    }
}

MainWindow::~MainWindow() {
    // Panels reference the controller and media services: destroy them (with the dock manager)
    // before Qt deletes the remaining children in creation order.
    delete m_dockManager; // NOLINT(cppcoreguidelines-owning-memory): Qt child, deleted early on purpose
    m_dockManager = nullptr;
}

ads::CDockWidget* MainWindow::dockWidget(ui::PanelId id) const noexcept {
    return m_docks[indexOf(id)];
}

void MainWindow::createDockWidgets() {
    for (const auto id : ui::kAllPanels) {
        auto* dock = new ads::CDockWidget(m_dockManager, toQString(ui::panelTitle(id)));
        dock->setObjectName(toQString(ui::panelObjectName(id)));
        dock->setWidget(createPanelWidget(id), ads::CDockWidget::ForceNoScrollArea);
        dock->setFeature(ads::CDockWidget::DockWidgetClosable, true);
        dock->setFeature(ads::CDockWidget::DockWidgetMovable, true);
        dock->setFeature(ads::CDockWidget::DockWidgetFloatable, true);
        m_docks[indexOf(id)] = dock;
    }

    using ui::PanelId;
    // Default "Editing" workspace, Premiere-like:
    //   [ Source | Effect Controls | Audio Track Mixer (tabs) ] [ Program ]
    //   [ Project ] [ Timeline                    ] [ Audio Meters ]
    auto* sourceArea = m_dockManager->addDockWidget(ads::TopDockWidgetArea, dockWidget(PanelId::SourceMonitor));
    m_dockManager->addDockWidgetTabToArea(dockWidget(PanelId::EffectControls), sourceArea);
    m_dockManager->addDockWidgetTabToArea(dockWidget(PanelId::AudioMixer), sourceArea);
    m_dockManager->addDockWidget(ads::RightDockWidgetArea, dockWidget(PanelId::ProgramMonitor), sourceArea);

    auto* timelineArea = m_dockManager->addDockWidget(ads::BottomDockWidgetArea, dockWidget(PanelId::Timeline));
    m_dockManager->addDockWidget(ads::LeftDockWidgetArea, dockWidget(PanelId::Project), timelineArea);
    m_dockManager->addDockWidget(ads::RightDockWidgetArea, dockWidget(PanelId::AudioMeters), timelineArea);

    sourceArea->setCurrentDockWidget(dockWidget(PanelId::SourceMonitor));
    // Relative widths of the bottom row (Project | Timeline | Audio Meters); QSplitter rescales them.
    m_dockManager->setSplitterSizes(timelineArea, {300, 900, 150});
}

QWidget* MainWindow::createPanelWidget(ui::PanelId id) {
    switch (id) {
    case ui::PanelId::Project: {
        m_projectPanel = new ui::ProjectPanel(*m_controller, *m_services);
        return m_projectPanel;
    }
    case ui::PanelId::EffectControls: {
        m_effectControls = new ui::EffectControlsPanel(*m_controller);
        return m_effectControls;
    }
    case ui::PanelId::Timeline: {
        m_timeline = new ui::timeline::TimelinePanel(*m_controller, *m_services);
        return m_timeline;
    }
    case ui::PanelId::ProgramMonitor: {
        m_programMonitor = new ui::ProgramMonitorPanel(*m_controller, m_playbackOptions);
        return m_programMonitor;
    }
    case ui::PanelId::SourceMonitor: {
        m_sourceMonitor = new ui::SourceMonitorPanel(*m_controller, *m_services, m_playbackOptions);
        return m_sourceMonitor;
    }
    case ui::PanelId::AudioMeters: {
        m_audioMeters = new ui::AudioMetersPanel();
        return m_audioMeters;
    }
    case ui::PanelId::AudioMixer: {
        m_audioMixer = new ui::AudioMixerPanel(*m_controller);
        return m_audioMixer;
    }
    default: return ui::createPanel(id);
    }
}

void MainWindow::createMenus() {
    auto* fileMenu = menuBar()->addMenu(tr("&File"));
    auto* newAction = fileMenu->addAction(tr("&New Project"));
    m_shortcuts->bind(newAction, QStringLiteral("app.new"), QString::fromLatin1(ui::ShortcutManager::kApplicationContext), QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_N));
    connect(newAction, &QAction::triggered, this, [this] {
        if (maybeSave()) {
            m_controller->newProject();
        }
    });
    auto* openAction = fileMenu->addAction(tr("&Open Project..."));
    m_shortcuts->bind(openAction, QStringLiteral("app.open"), QString::fromLatin1(ui::ShortcutManager::kApplicationContext), QKeySequence::Open);
    connect(openAction, &QAction::triggered, this, &MainWindow::openProject);
    fileMenu->addSeparator();
    auto* saveAction = fileMenu->addAction(tr("&Save"));
    m_shortcuts->bind(saveAction, QStringLiteral("app.save"), QString::fromLatin1(ui::ShortcutManager::kApplicationContext), QKeySequence::Save);
    connect(saveAction, &QAction::triggered, this, &MainWindow::save);
    auto* saveAsAction = fileMenu->addAction(tr("Save &As..."));
    m_shortcuts->bind(saveAsAction, QStringLiteral("app.saveAs"), QString::fromLatin1(ui::ShortcutManager::kApplicationContext), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S));
    connect(saveAsAction, &QAction::triggered, this, &MainWindow::saveAs);
    fileMenu->addSeparator();
    auto* quitAction = fileMenu->addAction(tr("E&xit"));
    m_shortcuts->bind(quitAction, QStringLiteral("app.quit"), QString::fromLatin1(ui::ShortcutManager::kApplicationContext), QKeySequence(Qt::CTRL | Qt::Key_Q));
    quitAction->setMenuRole(QAction::QuitRole);
    connect(quitAction, &QAction::triggered, this, &QWidget::close);

    fileMenu->insertAction(saveAction, m_projectPanel->importAction());
    fileMenu->insertSeparator(saveAction);

    auto* editMenu = menuBar()->addMenu(tr("&Edit"));
    editMenu->addAction(m_controller->undoAction());
    editMenu->addAction(m_controller->redoAction());
    m_shortcuts->bind(m_controller->undoAction(), QStringLiteral("app.undo"), QString::fromLatin1(ui::ShortcutManager::kApplicationContext), QKeySequence(Qt::CTRL | Qt::Key_Z));
    m_shortcuts->bind(m_controller->redoAction(), QStringLiteral("app.redo"), QString::fromLatin1(ui::ShortcutManager::kApplicationContext),
                      QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Z));
    m_shortcuts->bind(m_projectPanel->importAction(), QStringLiteral("project.import"), QStringLiteral("Project"),
                      QKeySequence(Qt::CTRL | Qt::Key_I));
    editMenu->addSeparator();
    auto* keyboard = editMenu->addAction(tr("&Keyboard Shortcuts..."));
    connect(keyboard, &QAction::triggered, this, [this] {
        ShortcutsDialog dialog(*m_shortcuts, this);
        dialog.exec();
    });
    auto* preferences = editMenu->addAction(tr("&Preferences..."));
    preferences->setMenuRole(QAction::PreferencesRole);
    connect(preferences, &QAction::triggered, this, [this] {
        PreferencesDialog dialog(*m_services, this);
        dialog.exec();
    });

    auto* windowMenu = menuBar()->addMenu(tr("&Window"));
    for (const auto id : ui::kAllPanels) {
        windowMenu->addAction(dockWidget(id)->toggleViewAction());
    }
}

void MainWindow::updateTitle() {
    // "[*]" is replaced by '*' while the window is marked modified.
    setWindowTitle(tr("%1[*] - Loki").arg(m_controller->displayName()));
    setWindowModified(m_controller->isModified());
}

QString MainWindow::projectFileFilter() {
    return tr("Loki Project (*%1)").arg(QString::fromUtf8(model::kProjectFileExtension.data(),
                                                         static_cast<qsizetype>(model::kProjectFileExtension.size())));
}

void MainWindow::openProject() {
    if (!maybeSave()) {
        return;
    }
    const auto path = QFileDialog::getOpenFileName(this, tr("Open Project"), QString(), projectFileFilter());
    if (!path.isEmpty() && !m_controller->openProject(path)) {
        statusBar()->showMessage(tr("Busy: another load or save is in progress"), 3000);
    }
}

bool MainWindow::save() {
    if (m_controller->filePath().isEmpty()) {
        return saveAs();
    }
    return m_controller->saveProject();
}

bool MainWindow::saveAs() {
    auto path = QFileDialog::getSaveFileName(this, tr("Save Project As"), m_controller->filePath(), projectFileFilter());
    if (path.isEmpty()) {
        return false;
    }
    const auto ext = QString::fromUtf8(model::kProjectFileExtension.data(),
                                       static_cast<qsizetype>(model::kProjectFileExtension.size()));
    if (!path.endsWith(ext, Qt::CaseInsensitive)) {
        path += ext;
    }
    return m_controller->saveProjectAs(path);
}

bool MainWindow::maybeSave() {
    if (!m_controller->isModified()) {
        return true;
    }
    const auto choice = QMessageBox::warning(
        this, tr("Unsaved Changes"), tr("Save changes to \"%1\"?").arg(m_controller->displayName()),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (choice == QMessageBox::Cancel) {
        return false;
    }
    if (choice == QMessageBox::Discard) {
        return true;
    }
    if (!save()) {
        return false;
    }
    m_controller->waitForPendingIo(); // brief: the write runs on a worker, we only wait for it
    return !m_controller->isModified();
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (maybeSave()) {
        event->accept();
    } else {
        event->ignore();
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
