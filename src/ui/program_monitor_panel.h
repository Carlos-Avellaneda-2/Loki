#pragma once

#include "model/ids.h"
#include "playback/playback_engine.h"
#include "ui/timeline/timeline_view.h"

#include <QWidget>

#include <memory>

class QAction;
class QComboBox;
class QHBoxLayout;
class QLabel;
class QTimer;
class QToolButton;

namespace loki::ui {

class ProjectController;
class ScrubBar;
class ShortcutManager;
class VideoView;

// Program Monitor: plays the active sequence. It owns the sequence playhead (the timeline reads
// and moves it through transport()). After every edit a new immutable SequenceSnapshot is
// published to the playback engine (coalesced: one snapshot per event-loop turn).
class ProgramMonitorPanel final : public QWidget {
    Q_OBJECT

public:
    ProgramMonitorPanel(ProjectController& controller, playback::PlaybackOptions options = {},
                        QWidget* parent = nullptr);
    ~ProgramMonitorPanel() override;

    ProgramMonitorPanel(const ProgramMonitorPanel&) = delete;
    ProgramMonitorPanel& operator=(const ProgramMonitorPanel&) = delete;
    ProgramMonitorPanel(ProgramMonitorPanel&&) = delete;
    ProgramMonitorPanel& operator=(ProgramMonitorPanel&&) = delete;

    void registerShortcuts(ShortcutManager& shortcuts);
    // Proxy/original toggle (D24): a shared checkable action shown as a toolbar button.
    void setProxyAction(QAction* action);
    void setUseProxies(bool on);
    [[nodiscard]] bool useProxies() const noexcept { return m_useProxies; }
    void setSequence(model::SequenceId id);
    [[nodiscard]] model::SequenceId sequenceId() const noexcept { return m_sequence; }
    [[nodiscard]] playback::PlaybackEngine& engine() noexcept { return *m_engine; }
    [[nodiscard]] timeline::Transport transport();

    // Publishes the current sequence state to the engine now (normally automatic).
    void publishSnapshot();
    void tick();
    [[nodiscard]] QString timecodeText() const;

private:
    void schedulePublish();
    void onQualityChanged(int index);

    ProjectController& m_controller;
    std::unique_ptr<playback::PlaybackEngine> m_engine;
    model::SequenceId m_sequence;
    std::uint64_t m_generation = 0;
    bool m_publishScheduled = false;

    VideoView* m_view = nullptr;
    ScrubBar* m_scrub = nullptr;
    QLabel* m_title = nullptr;
    QLabel* m_timecode = nullptr;
    QLabel* m_durationLabel = nullptr;
    QToolButton* m_playButton = nullptr;
    QComboBox* m_quality = nullptr;
    QHBoxLayout* m_info = nullptr;
    bool m_useProxies = false;
    QTimer* m_timer = nullptr;
};

} // namespace loki::ui
