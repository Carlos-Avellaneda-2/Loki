#pragma once

#include "commands/clip_commands.h"
#include "model/effect_registry.h"
#include "model/project.h"

#include <memory>
#include <string_view>

namespace loki::commands {

// Effect edits addressed by effect type (Motion, Opacity are unique per clip). The effect is created
// with its defaults on the first change (D12). Times are clip *source* times (keyframe time base).
// All of them throw CommandError for unknown clips/effects/parameters.

// Sets a parameter: a keyframe at `sourceTime` if the parameter is animated, else its static value.
// Edits with the same non-zero gesture merge (slider/spin-box drags).
[[nodiscard]] std::unique_ptr<Command> setEffectParameter(const model::Project& project, model::ClipId clip,
                                                          std::string_view effectType, std::string_view key,
                                                          double value, const Rational& sourceTime,
                                                          GestureId gesture = 0);

// Stopwatch: on = the current value becomes a keyframe at `sourceTime`; off = keyframes are removed
// and the value at `sourceTime` becomes the static value.
[[nodiscard]] std::unique_ptr<Command> setParameterAnimated(const model::Project& project, model::ClipId clip,
                                                            std::string_view effectType, std::string_view key, bool on,
                                                            const Rational& sourceTime);

// Adds a keyframe with the current value at `sourceTime`, or removes the one there.
[[nodiscard]] std::unique_ptr<Command> toggleKeyframe(const model::Project& project, model::ClipId clip,
                                                      std::string_view effectType, std::string_view key,
                                                      const Rational& sourceTime);

// Moves the keyframe at `from` to `to` (rejected if another keyframe is already there).
[[nodiscard]] std::unique_ptr<Command> moveKeyframe(const model::Project& project, model::ClipId clip,
                                                    std::string_view effectType, std::string_view key,
                                                    const Rational& from, const Rational& to, GestureId gesture = 0);

[[nodiscard]] std::unique_ptr<Command> setKeyframeInterpolation(const model::Project& project, model::ClipId clip,
                                                                std::string_view effectType, std::string_view key,
                                                                const Rational& time,
                                                                model::Interpolation interpolation);

// Removes the stored effect so every parameter returns to its default.
[[nodiscard]] std::unique_ptr<Command> resetEffect(const model::Project& project, model::ClipId clip,
                                                   std::string_view effectType);

// Sets a text parameter (title text, font, color, LUT file), creating the effect if needed.
[[nodiscard]] std::unique_ptr<Command> setEffectString(const model::Project& project, model::ClipId clip,
                                                       std::string_view effectType, std::string_view key,
                                                       std::string value);

// Adds a regular effect (Color Correction, LUT) with its defaults. Throws if the clip already has
// one of that type or if it does not fit the clip (video effect on audio, ...).
[[nodiscard]] std::unique_ptr<Command> applyEffect(const model::Project& project, model::ClipId clip,
                                                   std::string_view effectType);

// Media and sequence settings a clip's defaults depend on.
struct ClipContext {
    const model::Clip* clip = nullptr;
    model::MediaInfo media;
    model::SequenceSettings settings;
};
[[nodiscard]] ClipContext clipContext(const model::Project& project, model::ClipId clip);

} // namespace loki::commands
