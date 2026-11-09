#pragma once

#include "model/ids.h"
#include "playback/playback_engine.h"

#include <QWidget>

#include <memory>

class QComboBox;
class QLabel;
class QTimer;
class QToolButton;

namespace loki::ui {

class MediaServices;
class ProjectController;
class ScrubBar;
class VideoView;

// Source Monitor: plays one media item (double-click or drag from the Project panel), marks
// In/Out (stored on the media item through commands), J/K/L shuttle, frame stepping, scrubbing.
class SourceMonitorPanel final : public QWidget {
    Q_OBJECT

public:
    SourceMonitorPanel(ProjectController& controller, MediaServices& services,
                       playback::PlaybackOptions options = {}, QWidget* parent = nullptr);
    ~SourceMonitorPanel() override;

    SourceMonitorPanel(const SourceMonitorPanel&) = delete;
    SourceMonitorPanel& operator=(const SourceMonitorPanel&) = delete;
    SourceMonitorPanel(SourceMonitorPanel&&) = delete;
    SourceMonitorPanel& operator=(SourceMonitorPanel&&) = delete;

    void loadMedia(model::MediaId id);
    void unloadMedia();
    [[nodiscard]] model::MediaId mediaId() const noexcept { return m_media; }
    [[nodiscard]] playback::PlaybackEngine& engine() noexcept { return *m_engine; }

    // Transport (also bound to keyboard shortcuts while the panel has focus).
    void togglePlay();
    void shuttle(playback::ShuttleKey key);
    void step(int frames);
    void markIn();
    void markOut();
    void goToIn();
    void goToOut();
    void clearInOut();

    // Polls the engine and refreshes the view (called by a timer; public for tests).
    void tick();
    [[nodiscard]] QString timecodeText() const;

signals:
    // Insert (",") or Overwrite (".") the marked range into the active sequence at its playhead.
    void editRequested(loki::model::MediaId media, bool insert);

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    void createShortcuts();
    void refreshMarks();
    void onQualityChanged(int index);
    [[nodiscard]] const model::MediaItem* currentItem() const;
    [[nodiscard]] Rational currentFrameTime() const;

    ProjectController& m_controller;
    MediaServices& m_services;
    std::unique_ptr<playback::PlaybackEngine> m_engine;
    model::MediaId m_media;
    std::uint64_t m_seenOpenCount = 0;

    VideoView* m_view = nullptr;
    ScrubBar* m_scrub = nullptr;
    QLabel* m_title = nullptr;
    QLabel* m_timecode = nullptr;
    QLabel* m_durationLabel = nullptr;
    QToolButton* m_playButton = nullptr;
    QComboBox* m_quality = nullptr;
    QTimer* m_timer = nullptr;
};

} // namespace loki::ui
