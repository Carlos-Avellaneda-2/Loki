#pragma once

#include "playback/playback_engine.h"
#include "ui/panel_id.h"

#include <QByteArray>
#include <QMainWindow>

#include <array>
#include <cstddef>

class QCloseEvent;
class QSettings;

namespace loki::ui {
class AudioMetersPanel;
class EffectsPanel;
class AudioMixerPanel;
class MediaServices;
class ProjectController;
class ProjectPanel;
class SourceMonitorPanel;
class ShortcutManager;
class ProgramMonitorPanel;
class EffectControlsPanel;
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
    static constexpr int kLayoutVersion = 3; // 2: Audio Track Mixer panel, 3: Effects panel

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
    // File > New > Title: a title clip at the Program playhead on the targeted video track.
    void newTitle();
    // File > Export > Media (Ctrl+M).
    void exportMedia();
    [[nodiscard]] model::SequenceId activeSequence() const noexcept { return m_activeSequence; }
    // Insert/Overwrite from the Source Monitor at the Program playhead (public for tests).
    void sourceEdit(model::MediaId media, bool insert);

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void createDockWidgets();
    void createMenus();
    QWidget* createPanelWidget(ui::PanelId id);
    void activateFirstSequence();
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
    ui::AudioMetersPanel* m_audioMeters = nullptr;      // owned by its dock widget
    ui::AudioMixerPanel* m_audioMixer = nullptr;        // owned by its dock widget
    ui::EffectsPanel* m_effects = nullptr;              // owned by its dock widget
    model::SequenceId m_activeSequence;
    playback::PlaybackOptions m_playbackOptions;
    ads::CDockManager* m_dockManager = nullptr;    // owned by this window (Qt parent)
    std::array<ads::CDockWidget*, ui::kAllPanels.size()> m_docks{};
};

} // namespace loki::app
