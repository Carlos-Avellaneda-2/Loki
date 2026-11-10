#pragma once

#include "commands/clip_commands.h"
#include "commands/command.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace loki::commands {

// Captions (D40) and track automation (D39).

class SetSequenceCaptionsCommand final : public Command {
public:
    SetSequenceCaptionsCommand(model::SequenceId id, std::vector<model::Caption> captions, model::CaptionStyle style,
                               std::string text)
        : m_id(id), m_captions(std::move(captions)), m_style(std::move(style)), m_text(std::move(text)) {}
    [[nodiscard]] std::string text() const override { return m_text; }
    void redo(model::Project& project) override;
    void undo(model::Project& project) override;

private:
    model::SequenceId m_id;
    std::vector<model::Caption> m_captions;
    model::CaptionStyle m_style;
    std::vector<model::Caption> m_oldCaptions;
    model::CaptionStyle m_oldStyle;
    std::string m_text;
};

// Pure helpers on a sorted caption list.
// Inserts a caption at `start`: it is shortened so it does not run into the next one; returns
// nullopt when `start` falls inside an existing caption.
[[nodiscard]] std::optional<std::vector<model::Caption>> withCaption(std::vector<model::Caption> captions,
                                                                     model::Caption caption);
// Caption shown at `t`, if any.
[[nodiscard]] const model::Caption* captionAt(const std::vector<model::Caption>& captions, const Rational& t) noexcept;

[[nodiscard]] std::unique_ptr<Command> makeAddCaption(const model::Project& project, model::SequenceId id,
                                                      const Rational& start, const Rational& duration, std::string text,
                                                      model::CaptionId* added = nullptr);
// Replaces the caption with the same id (text, start, duration); refuses overlaps.
[[nodiscard]] std::unique_ptr<Command> makeEditCaption(const model::Project& project, model::SequenceId id,
                                                       model::Caption caption);
[[nodiscard]] std::unique_ptr<Command> makeRemoveCaption(const model::Project& project, model::SequenceId id,
                                                         model::CaptionId caption);
[[nodiscard]] std::unique_ptr<Command> makeSetCaptionStyle(const model::Project& project, model::SequenceId id,
                                                           model::CaptionStyle style);
// Replaces every caption with an imported list.
[[nodiscard]] std::unique_ptr<Command> makeReplaceCaptions(const model::Project& project, model::SequenceId id,
                                                           std::vector<model::Caption> captions);

// SubRip (.srt). Parsing tolerates CRLF, blank lines and "," or "." millisecond separators; it throws
// CommandError for malformed timings. Overlapping cues are trimmed so they do not overlap.
[[nodiscard]] std::vector<model::Caption> parseSrt(std::string_view text);
[[nodiscard]] std::string formatSrt(const std::vector<model::Caption>& captions);

// --- Track automation (D39) -----------------------------------------------------------------------

enum class AutomationKind { Volume, Pan };

// Adds a keyframe with the track's value at `t` (sequence time), or removes the one there. The first
// keyframe starts the automation (the static fader stays as the value before keyframes existed).
[[nodiscard]] std::unique_ptr<Command> toggleTrackAutomationKey(const model::Project& project, model::TrackId track,
                                                                AutomationKind kind, const Rational& t);
// Sets the value of the keyframe at `t` (creating it if needed). Gestures merge.
[[nodiscard]] std::unique_ptr<Command> setTrackAutomationValue(const model::Project& project, model::TrackId track,
                                                               AutomationKind kind, const Rational& t, double value,
                                                               GestureId gesture = 0);
// Moves the keyframe at `from` to `to`.
[[nodiscard]] std::unique_ptr<Command> moveTrackAutomationKey(const model::Project& project, model::TrackId track,
                                                              AutomationKind kind, const Rational& from,
                                                              const Rational& to, GestureId gesture = 0);
[[nodiscard]] std::unique_ptr<Command> clearTrackAutomation(const model::Project& project, model::TrackId track,
                                                            AutomationKind kind);

} // namespace loki::commands
