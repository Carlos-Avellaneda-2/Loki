#pragma once

#include "playback/playback_engine.h"
#include "ui/panel_id.h"

#include <QByteArray>
#include <QMainWindow>

#include <array>
#include <cstddef>

class QAction;
class QCloseEvent;
class QMenu;
class QSettings;
class QTimer;

namespace loki::ui {
class AudioMetersPanel;
class AutoSaver;
class StallDetector;
class EffectsPanel;
class AudioMixerPanel;
class MediaServices;
class ProjectController;
class ProjectPanel;
class SourceMonitorPanel;
class ShortcutManager;
class ProgramMonitorPanel;
class EffectControlsPanel;
class MarkersPanel;
class MaskOverlay;
class CaptionsPanel;
class ExportQueue;
class ExportQueuePanel;
class ScopesPanel;
namespace timeline {
class TimelinePanel;
} // namespace timeline
} // namespace loki::ui

namespace ads {
class CDockManager;
class CDockWidget;
} // namespace ads

namespace loki::app {

// Main window: hosts every panel as a dock widget (Qt Advanced Docking System) so panels can be
// docked, tabbed, resized and floated. Layout persistence is explicit (save/restoreSettings) so
// tests and alternate workspaces never touch the user's saved state implicitly.
class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    // Bump when the default panel set changes so stale saved layouts are ignored.
    static constexpr int kLayoutVersion = 5; // 2: Audio Track Mixer, 3: Effects, 4: Markers + Scopes, 5: Captions + Export Queue

    // playbackOptions: tests inject a fake audio device / clock.
    explicit MainWindow(QWidget* parent = nullptr, playback::PlaybackOptions playbackOptions = {});
    ~MainWindow() override;

    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;
    MainWindow(MainWindow&&) = delete;
    MainWindow& operator=(MainWindow&&) = delete;

    [[nodiscard]] ads::CDockManager* dockManager() const noexcept { return m_dockManager; }
    [[nodiscard]] ads::CDockWidget* dockWidget(ui::PanelId id) const noexcept;

    [[nodiscard]] QByteArray saveLayout() const;
    bool restoreLayout(const QByteArray& state);

    void saveSettings(QSettings& settings) const;
    // Returns true if a saved window geometry was applied (false on first run).
    bool restoreSettings(const QSettings& settings);

    [[nodiscard]] ui::ProjectController* projectController() const noexcept { return m_controller; }
    [[nodiscard]] ui::MediaServices* mediaServices() const noexcept { return m_services; }
    [[nodiscard]] ui::ShortcutManager* shortcuts() const noexcept { return m_shortcuts; }
    [[nodiscard]] ui::ProjectPanel* projectPanel() const noexcept { return m_projectPanel; }
    [[nodiscard]] ui::SourceMonitorPanel* sourceMonitor() const noexcept { return m_sourceMonitor; }
    [[nodiscard]] ui::ProgramMonitorPanel* programMonitor() const noexcept { return m_programMonitor; }
    [[nodiscard]] ui::EffectControlsPanel* effectControls() const noexcept { return m_effectControls; }
    [[nodiscard]] ui::timeline::TimelinePanel* timeline() const noexcept { return m_timeline; }
    [[nodiscard]] ui::AudioMetersPanel* audioMeters() const noexcept { return m_audioMeters; }
    [[nodiscard]] ui::AudioMixerPanel* audioMixer() const noexcept { return m_audioMixer; }
    [[nodiscard]] ui::EffectsPanel* effectsPanel() const noexcept { return m_effects; }
    [[nodiscard]] ui::MarkersPanel* markersPanel() const noexcept { return m_markers; }
    [[nodiscard]] ui::ScopesPanel* scopesPanel() const noexcept { return m_scopes; }
    [[nodiscard]] ui::MaskOverlay* maskOverlay() const noexcept { return m_maskOverlay; }
    [[nodiscard]] ui::CaptionsPanel* captionsPanel() const noexcept { return m_captions; }
    [[nodiscard]] ui::ExportQueue* exportQueue() const noexcept { return m_exportQueue; }
    // Sequence > Render In to Out (D42): queues the segments that are not rendered yet; returns how many.
    int renderPreviews();
    void deleteRenderFiles();
    [[nodiscard]] QString previewCacheDir() const;
    // Clip > Create Multi-Camera Source Sequence (D43) from the media selected in the Project panel.
    bool createMulticamSource(const std::vector<model::MediaId>& angles);
    // Cuts the multicam clip under the playhead to `angle` (keys 1-9).
    void switchCamera(int angle);
    // Sequence > Add Adjustment Layer (D31): at the playhead, above the targeted video track.
    void newAdjustmentLayer();
    // Sequence > Delete Sequence: also removes nested-sequence items that show it (D30).
    bool deleteSequence(model::SequenceId id);
    [[nodiscard]] QAction* proxyAction() const noexcept { return m_proxyAction; }
    // File > New > Title: a title clip at the Program playhead on the targeted video track.
    void newTitle();
    // File > Export > Media (Ctrl+M).
    void exportMedia();
    [[nodiscard]] ui::AutoSaver* autoSaver() const noexcept { return m_autoSaver; }
    [[nodiscard]] ui::StallDetector* stallDetector() const noexcept { return m_stallDetector; }
    // Offers to restore projects of sessions that crashed (D25). Called once after startup.
    void checkForRecovery();

    // Workspaces (D26): built-in Editing / Color / Audio layouts and user-saved ones (QSettings).
    enum class Workspace { Editing, Color, Audio };
    void applyWorkspace(Workspace workspace);
    bool saveWorkspace(const QString& name);
    bool applySavedWorkspace(const QString& name);
    void deleteWorkspace(const QString& name);
    [[nodiscard]] static QStringList savedWorkspaces();
    [[nodiscard]] model::SequenceId activeSequence() const noexcept { return m_activeSequence; }
    // Insert/Overwrite from the Source Monitor at the Program playhead (public for tests).
    void sourceEdit(model::MediaId media, bool insert);

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void createDockWidgets();
    void createMenus();
    void rebuildWorkspaceMenu();
    void showAllPanels();
    QWidget* createPanelWidget(ui::PanelId id);
    void activateFirstSequence();
    // Places a new generated item (title, adjustment layer) at the Program playhead.
    void placeGenerated(std::string_view generator, const std::string& name);
public:
    // Shows `id` in the Timeline, Program Monitor and mixer.
    void setActiveSequence(model::SequenceId id);
    // Sequence menu (public for tests): dialogs, then undoable commands.
    void newSequence();
    void sequenceSettings();
    void addTracks();
    // Creates a sequence and makes it active (no dialog).
    bool createSequence(const std::string& name, const model::SequenceSettings& settings, int videoTracks,
                        int audioTracks);
    bool addTracks(int video, int audio);
private:
    void updateTitle();
    void openProject();
    bool save();
    bool saveAs();
    // Asks to save unsaved changes. Returns false if the user cancelled.
    bool maybeSave();
    [[nodiscard]] static QString projectFileFilter();

    ui::ProjectController* m_controller = nullptr; // owned by this window (Qt parent)
    ui::MediaServices* m_services = nullptr;       // owned by this window (Qt parent)
    ui::ShortcutManager* m_shortcuts = nullptr;    // owned by this window (Qt parent)
    ui::ProjectPanel* m_projectPanel = nullptr;    // owned by its dock widget
    ui::SourceMonitorPanel* m_sourceMonitor = nullptr; // owned by its dock widget
    ui::ProgramMonitorPanel* m_programMonitor = nullptr; // owned by its dock widget
    ui::EffectControlsPanel* m_effectControls = nullptr; // owned by its dock widget
    ui::timeline::TimelinePanel* m_timeline = nullptr;  // owned by its dock widget
    ui::MarkersPanel* m_markers = nullptr;              // owned by its dock widget
    ui::ScopesPanel* m_scopes = nullptr;                // owned by its dock widget
    ui::MaskOverlay* m_maskOverlay = nullptr;           // owned by this window (Qt parent)
    ui::CaptionsPanel* m_captions = nullptr;            // owned by its dock widget
    ui::ExportQueue* m_exportQueue = nullptr;           // owned by this window (Qt parent)
    ui::ExportQueuePanel* m_exportQueuePanel = nullptr; // owned by its dock widget
    QTimer* m_previewStateTimer = nullptr;              // debounces the timeline render bar
    void updatePreviewStates();
    ui::AudioMetersPanel* m_audioMeters = nullptr;      // owned by its dock widget
    ui::AudioMixerPanel* m_audioMixer = nullptr;        // owned by its dock widget
    ui::EffectsPanel* m_effects = nullptr;              // owned by its dock widget
    QAction* m_proxyAction = nullptr;                   // owned by this window (Qt parent)
    ui::AutoSaver* m_autoSaver = nullptr;               // owned by this window (Qt parent)
    ui::StallDetector* m_stallDetector = nullptr;       // owned by this window (Qt parent)
    QMenu* m_workspaceMenu = nullptr;                   // owned by the menu bar
    model::SequenceId m_activeSequence;
    playback::PlaybackOptions m_playbackOptions;
    ads::CDockManager* m_dockManager = nullptr;    // owned by this window (Qt parent)
    std::array<ads::CDockWidget*, ui::kAllPanels.size()> m_docks{};
};

} // namespace loki::app
