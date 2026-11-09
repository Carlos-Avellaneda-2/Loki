#pragma once

#include "commands/clip_commands.h"
#include "commands/edit_commands.h"
#include "model/project.h"

#include <memory>
#include <string_view>

namespace loki::commands {

// Transitions (D19). Default duration when one is applied: 1 second.
inline const Rational kDefaultTransitionDuration{1};

class AddTransitionCommand final : public Command {
public:
    AddTransitionCommand(model::TrackId track, model::Transition transition)
        : m_track(track), m_transition(std::move(transition)) {}
    [[nodiscard]] std::string text() const override { return "Apply Transition"; }
    void redo(model::Project& project) override;
    void undo(model::Project& project) override;

private:
    model::TrackId m_track;
    model::Transition m_transition;
};

class RemoveTransitionCommand final : public Command {
public:
    explicit RemoveTransitionCommand(model::TransitionId id) : m_id(id) {}
    [[nodiscard]] std::string text() const override { return "Remove Transition"; }
    void redo(model::Project& project) override;
    void undo(model::Project& project) override;

private:
    model::TransitionId m_id;
    model::TrackId m_track;
    std::optional<model::Transition> m_removed;
};

// Replaces a transition's fields (same id). Merges within a gesture (duration drags).
class EditTransitionCommand final : public Command {
public:
    EditTransitionCommand(model::Transition after, GestureId gesture = 0)
        : m_after(std::move(after)), m_gesture(gesture) {}
    [[nodiscard]] std::string text() const override { return "Edit Transition"; }
    void redo(model::Project& project) override;
    void undo(model::Project& project) override;
    [[nodiscard]] int mergeId() const override { return m_gesture == 0 ? -1 : kMergeTransition; }
    bool mergeWith(const Command& next) override;

    static constexpr int kMergeTransition = 6;

private:
    model::Transition m_after;
    std::optional<model::Transition> m_before;
    GestureId m_gesture;
};

// Applies `type` at the `edge` of `clip`: a two-sided transition (centered on the cut) when a clip
// touches that edge on the same track, else a one-sided one (fade from/to nothing). Replaces a
// transition already at that place. Throws CommandError when the type does not fit the track.
[[nodiscard]] std::unique_ptr<Command> makeApplyTransition(const model::Project& project, model::ClipId clip,
                                                           ClipEdge edge, std::string_view type,
                                                           Rational duration = kDefaultTransitionDuration);

[[nodiscard]] std::unique_ptr<Command> setTransitionDuration(const model::Project& project, model::TransitionId id,
                                                             Rational duration, GestureId gesture = 0);
[[nodiscard]] std::unique_ptr<Command> setTransitionAlignment(const model::Project& project, model::TransitionId id,
                                                              model::TransitionAlignment alignment);

} // namespace loki::commands
