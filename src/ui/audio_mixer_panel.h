#pragma once

#include "commands/clip_commands.h"
#include "model/ids.h"
#include "playback/playback_engine.h"

#include <QWidget>

#include <functional>
#include <vector>

class QDial;
class QHBoxLayout;
class QLabel;
class QSlider;
class QTimer;
class QToolButton;

namespace loki::ui {

class LevelMeter;
class ProjectController;

// Audio Track Mixer (Premiere-like): one strip per audio track of the active sequence (pan knob,
// mute/solo, fader, meter) and a master strip. Every change is an undoable command; a fader or
// knob drag is one undo step (gesture).
class AudioMixerPanel final : public QWidget {
    Q_OBJECT

public:
    using LevelSource = std::function<playback::MixLevels()>;
    static constexpr double kFaderMaxDb = 6.0;
    static constexpr int kFaderSteps = 1000;

    explicit AudioMixerPanel(ProjectController& controller, QWidget* parent = nullptr);

    void setSequence(model::SequenceId id);
    void setLevelSource(LevelSource source) { m_levels = std::move(source); }
    void poll();

    // Strip access for tests.
    struct Strip {
        model::TrackId track; // null = master
        QLabel* name = nullptr;
        QDial* pan = nullptr;
        QToolButton* mute = nullptr;
        QToolButton* solo = nullptr;
        QSlider* fader = nullptr;
        QLabel* value = nullptr;
        LevelMeter* meter = nullptr;
    };
    [[nodiscard]] const std::vector<Strip>& strips() const noexcept { return m_strips; }
    [[nodiscard]] static int faderPosition(double db);
    [[nodiscard]] static double faderDb(int position);

private:
    void rebuild();
    void refreshValues();
    Strip makeStrip(model::TrackId track, const QString& name);
    void onFader(const Strip& strip, int position);
    void onPan(const Strip& strip, int value);

    ProjectController& m_controller;
    model::SequenceId m_sequence;
    LevelSource m_levels;
    std::vector<Strip> m_strips; // tracks, then master
    std::vector<model::TrackId> m_builtFor;
    QHBoxLayout* m_row = nullptr;
    QTimer* m_timer = nullptr;
    commands::GestureId m_gesture = 0;
    bool m_updating = false;
};

} // namespace loki::ui
