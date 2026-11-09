#pragma once

#include "commands/clip_commands.h"
#include "model/effect_registry.h"
#include "model/ids.h"

#include <QWidget>

#include <chrono>
#include <functional>
#include <optional>
#include <vector>

class QComboBox;
class QDoubleSpinBox;
class QFontComboBox;
class QLineEdit;
class QLabel;
class QTimer;
class QToolButton;
class QVBoxLayout;

namespace loki::ui {

class ProjectController;
class KeyframeLane;

// Effect Controls: Motion and Opacity (and later effects) of the selected clip. Each parameter has
// a stopwatch (animate on/off), a value editor, previous/add-remove/next keyframe buttons and a
// lane with the clip's keyframes (drag to move, right-click for interpolation). Values follow the
// sequence playhead; every change is an undoable command.
class EffectControlsPanel final : public QWidget {
    Q_OBJECT

public:
    struct Playhead {
        std::function<Rational()> position; // sequence time
        std::function<void(const Rational&)> seek;
    };

    EffectControlsPanel(ProjectController& controller, QWidget* parent = nullptr);

    void setPlayhead(Playhead playhead) { m_playhead = std::move(playhead); }
    void setClip(model::ClipId clip);
    [[nodiscard]] model::ClipId clip() const noexcept { return m_clip; }
    // Shows a transition (duration, alignment) instead of a clip.
    void setTransition(model::TransitionId transition);
    [[nodiscard]] model::TransitionId transition() const noexcept { return m_transition; }

    // Clip source time at the playhead (clamped into the clip) - the keyframe time base.
    [[nodiscard]] Rational playheadSourceTime() const;
    // Refreshes displayed values for the current playhead (called by a timer; public for tests).
    void refreshValues();

    // Edits used by the row widgets (public for tests).
    void setValue(const std::string& effectType, const std::string& key, double value, commands::GestureId gesture);
    void setAnimated(const std::string& effectType, const std::string& key, bool on);
    void toggleKeyframe(const std::string& effectType, const std::string& key);
    void jumpKeyframe(const std::string& effectType, const std::string& key, bool forward);
    void resetEffect(const std::string& effectType);
    void setString(const std::string& effectType, const std::string& key, const std::string& value);
    // Effect types shown for the current clip, in order (tests).
    [[nodiscard]] std::vector<std::string> shownEffects() const;

private:
    struct Row {
        std::string effectType;
        std::string key;
        const model::ParameterDef* def = nullptr;
        QToolButton* stopwatch = nullptr;
        QDoubleSpinBox* spin = nullptr;
        QComboBox* combo = nullptr;
        KeyframeLane* lane = nullptr;
        // Text parameters.
        const model::StringParameterDef* stringDef = nullptr;
        QLineEdit* line = nullptr;
        QFontComboBox* font = nullptr;
        QToolButton* color = nullptr;
    };
    [[nodiscard]] std::vector<std::string> effectTypesFor(const model::Clip& clip) const;
    void rebuildTransition();

    void rebuild();
    [[nodiscard]] const model::Clip* currentClip() const;
    [[nodiscard]] commands::GestureId gestureFor(const void* editor);
    void run(std::unique_ptr<commands::Command> command);

    ProjectController& m_controller;
    Playhead m_playhead;
    model::ClipId m_clip;
    QLabel* m_title = nullptr;
    QWidget* m_body = nullptr;
    QVBoxLayout* m_bodyLayout = nullptr;
    std::vector<Row> m_rows;
    std::vector<std::string> m_shownTypes;
    model::TransitionId m_transition;
    QDoubleSpinBox* m_transitionDuration = nullptr;
    QComboBox* m_transitionAlignment = nullptr;
    QTimer* m_timer = nullptr;
    Rational m_lastPlayhead{-1};
    bool m_updating = false;
    const void* m_gestureEditor = nullptr;
    commands::GestureId m_gesture = 0;
    std::chrono::steady_clock::time_point m_gestureTime;
};

// Keyframe track of one parameter over the clip's source range.
class KeyframeLane final : public QWidget {
    Q_OBJECT

public:
    KeyframeLane(ProjectController& controller, EffectControlsPanel& panel, std::string effectType, std::string key,
                 QWidget* parent = nullptr);

    void setRange(model::ClipId clip, const Rational& sourceIn, const Rational& sourceOut);
    void setPlayheadSourceTime(const Rational& t);
    [[nodiscard]] QSize sizeHint() const override { return {220, 22}; }

signals:
    void seekRequested(const loki::Rational& sourceTime);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;

private:
    [[nodiscard]] double xOf(const Rational& t) const;
    [[nodiscard]] Rational timeAt(double x) const;
    [[nodiscard]] std::optional<Rational> keyframeAt(double x) const;
    [[nodiscard]] const model::Parameter* parameter() const;

    ProjectController& m_controller;
    EffectControlsPanel& m_panel;
    std::string m_effectType;
    std::string m_key;
    model::ClipId m_clip;
    Rational m_in;
    Rational m_out;
    Rational m_playhead;
    std::optional<Rational> m_dragFrom;
    std::optional<Rational> m_dragTo;
};

} // namespace loki::ui
