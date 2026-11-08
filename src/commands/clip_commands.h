#pragma once

#include "commands/command.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace loki::commands {

// Merge ids for commands that collapse into one undo step while dragging.
enum MergeId : int {
    kMergeMoveClip = 1,
    kMergeEditClip = 2,
};

// Identifies one continuous user gesture (a drag, a slider scrub). Commands created with the same
// non-zero gesture merge into one undo step; 0 means "never merge". The UI hands out ids.
using GestureId = std::uint64_t;

// All clip commands refuse to touch clips on locked tracks (checked on first execution only, so
// undo/redo always replays faithfully).

class AddClipCommand final : public Command {
public:
    AddClipCommand(model::TrackId track, model::Clip clip);
    [[nodiscard]] std::string text() const override { return "Add Clip"; }
    void redo(model::Project& project) override;
    void undo(model::Project& project) override;
    [[nodiscard]] model::ClipId clipId() const noexcept { return m_clip.id; }

private:
    model::TrackId m_track;
    model::Clip m_clip;
    bool m_checked = false;
};

class RemoveClipCommand final : public Command {
public:
    explicit RemoveClipCommand(model::ClipId id) : m_id(id) {}
    [[nodiscard]] std::string text() const override { return "Delete Clip"; }
    void redo(model::Project& project) override;
    void undo(model::Project& project) override;

private:
    model::ClipId m_id;
    model::TrackId m_track;
    std::optional<model::Clip> m_clip;
};

// Moves a clip to `newStart` on `targetTrack` (same kind of track). Moves of the same clip within
// one gesture merge into one undo step.
class MoveClipCommand final : public Command {
public:
    MoveClipCommand(model::ClipId id, model::TrackId targetTrack, Rational newStart, GestureId gesture = 0);
    [[nodiscard]] std::string text() const override { return "Move Clip"; }
    void redo(model::Project& project) override;
    void undo(model::Project& project) override;
    [[nodiscard]] int mergeId() const override { return kMergeMoveClip; }
    bool mergeWith(const Command& next) override;

private:
    static void moveTo(model::Project& project, model::ClipId id, model::TrackId track, const Rational& start);

    model::ClipId m_id;
    model::TrackId m_targetTrack;
    Rational m_newStart;
    GestureId m_gesture;
    model::TrackId m_oldTrack;
    Rational m_oldStart;
    bool m_checked = false;
};

// Splits a clip at sequence time `at` (strictly inside the clip). The left part keeps the id;
// the right part gets `rightId` (generated when null).
class SplitClipCommand final : public Command {
public:
    SplitClipCommand(model::ClipId id, Rational at, model::ClipId rightId = {});
    [[nodiscard]] std::string text() const override { return "Split Clip"; }
    void redo(model::Project& project) override;
    void undo(model::Project& project) override;
    [[nodiscard]] model::ClipId rightClipId() const noexcept { return m_rightId; }

private:
    model::ClipId m_id;
    Rational m_at;
    model::ClipId m_rightId;
    std::optional<model::Clip> m_original;
};

// Generic before/after clip edit (trim, enable, rename, effects, parameters, keyframes).
// Edits with the same non-empty merge key on the same clip merge into one undo step; factories
// build the key from the edited property and the gesture id.
class EditClipCommand final : public Command {
public:
    using Edit = std::function<void(model::Clip&)>;
    EditClipCommand(model::ClipId id, std::string text, Edit edit, std::string mergeKey = {});
    [[nodiscard]] std::string text() const override { return m_text; }
    void redo(model::Project& project) override;
    void undo(model::Project& project) override;
    [[nodiscard]] int mergeId() const override { return m_mergeKey.empty() ? -1 : kMergeEditClip; }
    bool mergeWith(const Command& next) override;

private:
    model::ClipId m_id;
    std::string m_text;
    Edit m_edit;
    std::string m_mergeKey;
    std::optional<model::Clip> m_before;
    std::optional<model::Clip> m_after;
};

// Sets the clip's timeline range and source in-point (trim). Merges within a gesture.
[[nodiscard]] std::unique_ptr<Command> trimClip(model::ClipId id, Rational start, Rational sourceIn, Rational duration,
                                                GestureId gesture = 0);
[[nodiscard]] std::unique_ptr<Command> setClipEnabled(model::ClipId id, bool enabled);
[[nodiscard]] std::unique_ptr<Command> renameClip(model::ClipId id, std::string name);
[[nodiscard]] std::unique_ptr<Command> addEffect(model::ClipId id, model::Effect effect);
[[nodiscard]] std::unique_ptr<Command> removeEffect(model::ClipId id, model::EffectId effect);
[[nodiscard]] std::unique_ptr<Command> setEffectEnabled(model::ClipId id, model::EffectId effect, bool enabled);
// Static value of a parameter (no keyframes). Merges within a gesture (slider drag).
[[nodiscard]] std::unique_ptr<Command> setParameterValue(model::ClipId id, model::EffectId effect, std::string key,
                                                         double value, GestureId gesture = 0);
// Adds a keyframe, or replaces the one at the same time.
[[nodiscard]] std::unique_ptr<Command> setKeyframe(model::ClipId id, model::EffectId effect, std::string key,
                                                   model::Keyframe keyframe);
[[nodiscard]] std::unique_ptr<Command> removeKeyframe(model::ClipId id, model::EffectId effect, std::string key,
                                                      Rational time);

} // namespace loki::commands
