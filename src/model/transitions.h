#pragma once

#include "model/types.h"

#include <optional>
#include <string_view>
#include <vector>

namespace loki::model {

// Built-in transitions (D19).
inline constexpr std::string_view kCrossDissolve = "loki.crossDissolve";
inline constexpr std::string_view kDipToBlack = "loki.dipToBlack";
inline constexpr std::string_view kWipe = "loki.wipe";
inline constexpr std::string_view kCrossfade = "loki.crossfade"; // audio, constant power

struct TransitionDef {
    std::string_view type;
    std::string_view label;
    TrackKind kind;
};
[[nodiscard]] const std::vector<TransitionDef>& transitionRegistry();
[[nodiscard]] const TransitionDef* findTransitionDef(std::string_view type) noexcept;

// Where a transition acts on its track, or nullopt when it is dormant (its clips are gone or no
// longer touch). `from`/`to` point into `track` (null for a one-sided transition).
struct TransitionSpan {
    Rational start;
    Rational end;
    const Clip* from = nullptr;
    const Clip* to = nullptr;

    [[nodiscard]] bool contains(const Rational& t) const { return start <= t && t < end; }
    // 0 at the start of the span, 1 at its end.
    [[nodiscard]] double progress(const Rational& t) const;
};
[[nodiscard]] std::optional<TransitionSpan> transitionSpan(const Track& track, const Transition& transition);

// Active transition span on `track` at `t` (the first one if several overlap).
struct ActiveTransition {
    const Transition* transition = nullptr;
    TransitionSpan span;
};
[[nodiscard]] std::optional<ActiveTransition> activeTransition(const Track& track, const Rational& t);

// Transitions of `track` whose span is valid (helper for edits that clean up dormant ones).
[[nodiscard]] bool isTransitionLive(const Track& track, const Transition& transition);

} // namespace loki::model
