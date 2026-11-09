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
#include "ui/effects_panel.h"
#include "app/export_dialog.h"
#include "ui/auto_saver.h"
#include "ui/stall_detector.h"
#include "commands/effect_commands.h"
#include "commands/transition_commands.h"
#include "ui/program_monitor_panel.h"
#include "ui/source_monitor_panel.h"
#include "ui/timeline/timeline_panel.h"
#include "util/timecode.h"

#include <DockAreaWidget.h>
#include <DockManager.h>
#include <DockWidget.h>

#include <QDir>
#include <QFileInfo>
#include <QLocale>
#include <QInputDialog>
#include <QLineEdit>
#include <QAction>

#include <algorithm>
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
    // Logs UI blocks > 50 ms (performance criterion); a 20 ms tick keeps idle wake-ups low.
    m_stallDetector = new ui::StallDetector(this, 20);
    m_autoSaver = new ui::AutoSaver(*m_controller, QDir(m_services->cacheDirectory()).filePath(QStringLiteral("autosave")),
                                    ui::AutoSaver::loadSettings(), this);
    // Proxy/original toggle shared by both monitors (D24); an app preference, not project data.
    m_proxyAction = new QAction(tr("Proxy"), this);
    m_proxyAction->setCheckable(true);
    m_proxyAction->setToolTip(tr("Toggle Proxies: play 720p proxies where they exist (export always uses originals)"));
    m_shortcuts->bind(m_proxyAction, QStringLiteral("app.toggleProxies"),
                      QString::fromLatin1(ui::ShortcutManager::kApplicationContext), QKeySequence());
    connect(m_proxyAction, &QAction::toggled, this, [this](bool on) {
        QSettings().setValue(QStringLiteral("playback/useProxies"), on);
        m_programMonitor->setUseProxies(on);
        m_sourceMonitor->setUseProxies(on);
    });
    m_programMonitor->setProxyAction(m_proxyAction);
    m_sourceMonitor->setProxyAction(m_proxyAction);
    m_proxyAction->setChecked(QSettings().value(QStringLiteral("playback/useProxies"), false).toBool());
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
    connect(m_timeline->view(), &ui::timeline::TimelineView::transitionSelected, this, [this](model::TransitionId id) {
        if (id) {
            m_effectControls->setTransition(id);
            dockWidget(ui::PanelId::EffectControls)->raise();
        } else if (m_effectControls->transition()) {
            m_effectControls->setClip({});
        }
    });
    connect(m_effects, &ui::EffectsPanel::applyRequested, this, [this](const QString& type, bool isTransition) {
        if (isTransition) {
            m_timeline->view()->applyTransitionAtPlayhead(type.toStdString());
        } else {
            m_timeline->view()->applyEffectToSelection(type.toStdString());
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

void MainWindow::checkForRecovery() {
    auto orphans = ui::AutoSaver::findOrphans(m_autoSaver->folder());
    if (orphans.empty()) {
        return;
    }
    std::sort(orphans.begin(), orphans.end(), [](const auto& a, const auto& b) { return a.saved > b.saved; });
    const auto& newest = orphans.front();
    const QString name = newest.originalPath.isEmpty() ? tr("an untitled project") : QFileInfo(newest.originalPath).fileName();
    const auto answer = QMessageBox::question(
        this, tr("Recover Project"),
        tr("Loki did not close normally. Unsaved changes to %1 from %2 were recovered.\n\nOpen them now?")
            .arg(name, QLocale().toString(newest.saved, QLocale::ShortFormat)),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
    if (answer == QMessageBox::Yes) {
        const auto recovery = newest;
        auto connection = std::make_shared<QMetaObject::Connection>();
        *connection = connect(m_controller, &ui::ProjectController::openFinished, this,
                              [recovery, connection](bool ok, const QString&) {
                                  QObject::disconnect(*connection);
                                  // Opened: the live session protects it now. Failed: keep a copy.
                                  ok ? ui::AutoSaver::discard(recovery) : (void)ui::AutoSaver::keepAsAutosave(recovery);
                              });
        if (m_controller->openRecovered(newest.file, newest.originalPath)) {
            orphans.erase(orphans.begin());
        } else {
            disconnect(*connection);
        }
    }
    // Not recovered now (declined, or older sessions): never lose them, keep them as autosave copies.
    for (const auto& other : orphans) {
        const auto kept = ui::AutoSaver::keepAsAutosave(other);
        if (!kept.isEmpty()) {
            statusBar()->showMessage(tr("Unsaved changes were kept in %1").arg(QDir::toNativeSeparators(kept)), 10000);
        }
    }
}

void MainWindow::exportMedia() {
    if (m_controller->project().findSequence(m_activeSequence) == nullptr) {
        statusBar()->showMessage(tr("No active sequence"), 4000);
        return;
    }
    ExportDialog dialog(*m_controller, m_activeSequence, this);
    connect(&dialog, &ExportDialog::exportFinished, this,
            [this](bool, const QString& message) { statusBar()->showMessage(message, 8000); });
    dialog.exec();
}

void MainWindow::newTitle() {
    const auto& project = m_controller->project();
    const auto* sequence = project.findSequence(m_activeSequence);
    if (sequence == nullptr || sequence->videoTracks.empty()) {
        statusBar()->showMessage(tr("No active sequence"), 4000);
        return;
    }
    const Rational at = quantizeToFrame(m_programMonitor->engine().position(), sequence->settings.frameRate,
                                        Rounding::Floor);
    const Rational duration(5);
    // Like Premiere: the first unlocked video track above the targeted one that is free over the
    // title's range; the top track if none is.
    const auto& tracks = sequence->videoTracks;
    std::size_t from = 0;
    for (std::size_t i = 0; i < tracks.size(); ++i) {
        if (tracks[i].id == m_timeline->view()->targets().video) {
            from = i + 1;
        }
    }
    auto track = tracks.back().id;
    for (std::size_t i = std::min(from, tracks.size() - 1); i < tracks.size(); ++i) {
        const bool free = std::none_of(tracks[i].clips.begin(), tracks[i].clips.end(), [&](const model::Clip& c) {
            return c.start < at + duration && at < c.end();
        });
        if (free && !tracks[i].state.locked) {
            track = tracks[i].id;
            break;
        }
    }
    try {
        model::MediaId media;
        auto command = commands::makeNewTitle(project, m_activeSequence, track, at, duration, "Title", &media);
        if (m_controller->execute(std::move(command))) {
            for (const auto& c : project.findTrack(track).track->clips) {
                if (c.media == media) {
                    m_timeline->view()->setSelection({c.id});
                    break;
                }
            }
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

    applyWorkspace(Workspace::Editing);
}

void MainWindow::showAllPanels() {
    for (const auto id : ui::kAllPanels) {
        if (dockWidget(id)->isClosed()) {
            dockWidget(id)->toggleView(true);
        }
    }
}

void MainWindow::applyWorkspace(Workspace workspace) {
    using ui::PanelId;
    // Take every panel out, then dock them again in the workspace's arrangement.
    for (const auto id : ui::kAllPanels) {
        auto* dock = dockWidget(id);
        if (dock->dockAreaWidget() != nullptr) {
            m_dockManager->removeDockWidget(dock);
        }
    }
    if (workspace == Workspace::Color) {
        //   [ Effect Controls ] [ Program (large)             ]
        //   [ Project | Effects ] [ Timeline ] [ Audio Meters ]
        auto* programArea = m_dockManager->addDockWidget(ads::TopDockWidgetArea, dockWidget(PanelId::ProgramMonitor));
        auto* controlsArea =
            m_dockManager->addDockWidget(ads::LeftDockWidgetArea, dockWidget(PanelId::EffectControls), programArea);
        m_dockManager->addDockWidgetTabToArea(dockWidget(PanelId::SourceMonitor), controlsArea);
        m_dockManager->addDockWidgetTabToArea(dockWidget(PanelId::AudioMixer), controlsArea);
        controlsArea->setCurrentDockWidget(dockWidget(PanelId::EffectControls));
        auto* timelineArea = m_dockManager->addDockWidget(ads::BottomDockWidgetArea, dockWidget(PanelId::Timeline));
        auto* effectsArea = m_dockManager->addDockWidget(ads::LeftDockWidgetArea, dockWidget(PanelId::Effects), timelineArea);
        m_dockManager->addDockWidgetTabToArea(dockWidget(PanelId::Project), effectsArea);
        effectsArea->setCurrentDockWidget(dockWidget(PanelId::Effects));
        m_dockManager->addDockWidget(ads::RightDockWidgetArea, dockWidget(PanelId::AudioMeters), timelineArea);
        m_dockManager->setSplitterSizes(programArea, {350, 950});
        showAllPanels();
        return;
    }
    if (workspace == Workspace::Audio) {
        //   [ Audio Track Mixer | Source ] [ Program ]
        //   [ Project | Effects ] [ Timeline ] [ Audio Meters (wide) ]
        auto* mixerArea = m_dockManager->addDockWidget(ads::TopDockWidgetArea, dockWidget(PanelId::AudioMixer));
        m_dockManager->addDockWidgetTabToArea(dockWidget(PanelId::SourceMonitor), mixerArea);
        m_dockManager->addDockWidgetTabToArea(dockWidget(PanelId::EffectControls), mixerArea);
        mixerArea->setCurrentDockWidget(dockWidget(PanelId::AudioMixer));
        m_dockManager->addDockWidget(ads::RightDockWidgetArea, dockWidget(PanelId::ProgramMonitor), mixerArea);
        auto* timelineArea = m_dockManager->addDockWidget(ads::BottomDockWidgetArea, dockWidget(PanelId::Timeline));
        auto* projectArea = m_dockManager->addDockWidget(ads::LeftDockWidgetArea, dockWidget(PanelId::Project), timelineArea);
        m_dockManager->addDockWidgetTabToArea(dockWidget(PanelId::Effects), projectArea);
        projectArea->setCurrentDockWidget(dockWidget(PanelId::Project));
        m_dockManager->addDockWidget(ads::RightDockWidgetArea, dockWidget(PanelId::AudioMeters), timelineArea);
        m_dockManager->setSplitterSizes(timelineArea, {250, 800, 250});
        showAllPanels();
        return;
    }
    // Default "Editing" workspace, Premiere-like:
    //   [ Source | Effect Controls | Audio Track Mixer (tabs) ] [ Program ]
    //   [ Project ] [ Timeline                    ] [ Audio Meters ]
    auto* sourceArea = m_dockManager->addDockWidget(ads::TopDockWidgetArea, dockWidget(PanelId::SourceMonitor));
    m_dockManager->addDockWidgetTabToArea(dockWidget(PanelId::EffectControls), sourceArea);
    m_dockManager->addDockWidgetTabToArea(dockWidget(PanelId::AudioMixer), sourceArea);
    m_dockManager->addDockWidget(ads::RightDockWidgetArea, dockWidget(PanelId::ProgramMonitor), sourceArea);

    auto* timelineArea = m_dockManager->addDockWidget(ads::BottomDockWidgetArea, dockWidget(PanelId::Timeline));
    auto* projectArea = m_dockManager->addDockWidget(ads::LeftDockWidgetArea, dockWidget(PanelId::Project), timelineArea);
    m_dockManager->addDockWidgetTabToArea(dockWidget(PanelId::Effects), projectArea);
    projectArea->setCurrentDockWidget(dockWidget(PanelId::Project));
    m_dockManager->addDockWidget(ads::RightDockWidgetArea, dockWidget(PanelId::AudioMeters), timelineArea);

    sourceArea->setCurrentDockWidget(dockWidget(PanelId::SourceMonitor));
    // Relative widths of the bottom row (Project | Timeline | Audio Meters); QSplitter rescales them.
    m_dockManager->setSplitterSizes(timelineArea, {300, 900, 150});
    showAllPanels();
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
    case ui::PanelId::Effects: {
        m_effects = new ui::EffectsPanel();
        return m_effects;
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
    auto* titleAction = new QAction(tr("New &Title"), this);
    m_shortcuts->bind(titleAction, QStringLiteral("app.newTitle"), QString::fromLatin1(ui::ShortcutManager::kApplicationContext),
                      QKeySequence(Qt::CTRL | Qt::Key_T));
    connect(titleAction, &QAction::triggered, this, &MainWindow::newTitle);
    fileMenu->insertAction(saveAction, titleAction);
    auto* exportAction = new QAction(tr("&Export Media..."), this);
    m_shortcuts->bind(exportAction, QStringLiteral("app.exportMedia"), QString::fromLatin1(ui::ShortcutManager::kApplicationContext),
                      QKeySequence(Qt::CTRL | Qt::Key_M));
    connect(exportAction, &QAction::triggered, this, &MainWindow::exportMedia);
    fileMenu->insertAction(saveAction, exportAction);
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
        PreferencesDialog dialog(*m_services, m_autoSaver, this);
        dialog.exec();
    });

    auto* windowMenu = menuBar()->addMenu(tr("&Window"));
    m_workspaceMenu = windowMenu->addMenu(tr("&Workspaces"));
    rebuildWorkspaceMenu();
    windowMenu->addSeparator();
    for (const auto id : ui::kAllPanels) {
        windowMenu->addAction(dockWidget(id)->toggleViewAction());
    }
}

void MainWindow::rebuildWorkspaceMenu() {
    m_workspaceMenu->clear();
    const std::pair<Workspace, QString> builtIn[] = {
        {Workspace::Editing, tr("Editing")}, {Workspace::Color, tr("Color")}, {Workspace::Audio, tr("Audio")}};
    for (const auto& [w, label] : builtIn) {
        m_workspaceMenu->addAction(label, this, [this, w = w] { applyWorkspace(w); });
    }
    const auto saved = savedWorkspaces();
    if (!saved.isEmpty()) {
        m_workspaceMenu->addSeparator();
        for (const auto& name : saved) {
            m_workspaceMenu->addAction(name, this, [this, name] { applySavedWorkspace(name); });
        }
    }
    m_workspaceMenu->addSeparator();
    m_workspaceMenu->addAction(tr("Save as New Workspace..."), this, [this] {
        bool ok = false;
        const auto name = QInputDialog::getText(this, tr("New Workspace"), tr("Name:"), QLineEdit::Normal, QString(), &ok);
        if (ok && !name.trimmed().isEmpty()) {
            saveWorkspace(name.trimmed());
        }
    });
    if (!saved.isEmpty()) {
        auto* remove = m_workspaceMenu->addMenu(tr("Delete Workspace"));
        for (const auto& name : saved) {
            remove->addAction(name, this, [this, name] { deleteWorkspace(name); });
        }
    }
}

QStringList MainWindow::savedWorkspaces() {
    QSettings settings;
    settings.beginGroup(QStringLiteral("workspaces"));
    auto names = settings.childKeys();
    names.sort(Qt::CaseInsensitive);
    return names;
}

bool MainWindow::saveWorkspace(const QString& name) {
    if (name.isEmpty()) {
        return false;
    }
    QSettings().setValue(QStringLiteral("workspaces/") + name, saveLayout());
    rebuildWorkspaceMenu();
    return true;
}

bool MainWindow::applySavedWorkspace(const QString& name) {
    return restoreLayout(QSettings().value(QStringLiteral("workspaces/") + name).toByteArray());
}

void MainWindow::deleteWorkspace(const QString& name) {
    QSettings().remove(QStringLiteral("workspaces/") + name);
    rebuildWorkspaceMenu();
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

