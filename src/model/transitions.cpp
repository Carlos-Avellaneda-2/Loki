#include "model/transitions.h"

#include <algorithm>

namespace loki::model {

const std::vector<TransitionDef>& transitionRegistry() {
    static const std::vector<TransitionDef> kRegistry = {
        {kCrossDissolve, "Cross Dissolve", TrackKind::Video},
        {kDipToBlack, "Dip to Black", TrackKind::Video},
        {kWipe, "Wipe", TrackKind::Video},
        {kCrossfade, "Constant Power", TrackKind::Audio},
    };
    return kRegistry;
}

const TransitionDef* findTransitionDef(std::string_view type) noexcept {
    for (const auto& def : transitionRegistry()) {
        if (def.type == type) {
            return &def;
        }
    }
    return nullptr;
}

double TransitionSpan::progress(const Rational& t) const {
    const Rational length = end - start;
    if (!length.isPositive()) {
        return 1.0;
    }
    return std::clamp(((t - start) / length).toDouble(), 0.0, 1.0);
}

std::optional<TransitionSpan> transitionSpan(const Track& track, const Transition& transition) {
    if (!transition.duration.isPositive()) {
        return std::nullopt;
    }
    TransitionSpan span;
    span.from = transition.from ? track.findClip(transition.from) : nullptr;
    span.to = transition.to ? track.findClip(transition.to) : nullptr;
    if ((transition.from && span.from == nullptr) || (transition.to && span.to == nullptr) ||
        (span.from == nullptr && span.to == nullptr)) {
        return std::nullopt;
    }
    const Rational d = transition.duration;
    if (span.from != nullptr && span.to != nullptr) {
        if (span.from->end() != span.to->start) {
            return std::nullopt;
        }
        const Rational cut = span.to->start;
        switch (transition.alignment) {
        case TransitionAlignment::Center:
            span.start = cut - d / Rational(2);
            span.end = cut + d / Rational(2);
            break;
        case TransitionAlignment::StartAtCut:
            span.start = cut;
            span.end = cut + d;
            break;
        case TransitionAlignment::EndAtCut:
            span.start = cut - d;
            span.end = cut;
            break;
        }
        span.start = max(span.start, span.from->start);
        span.end = min(span.end, span.to->end());
    } else if (span.from != nullptr) {
        span.end = span.from->end();
        span.start = max(span.from->start, span.end - d);
    } else {
        span.start = span.to->start;
        span.end = min(span.to->end(), span.start + d);
    }
    if (!(span.start < span.end)) {
        return std::nullopt;
    }
    return span;
}

std::optional<ActiveTransition> activeTransition(const Track& track, const Rational& t) {
    for (const auto& transition : track.transitions) {
        if (const auto span = transitionSpan(track, transition); span && span->contains(t)) {
            return ActiveTransition{&transition, *span};
        }
    }
    return std::nullopt;
}

bool isTransitionLive(const Track& track, const Transition& transition) {
    return transitionSpan(track, transition).has_value();
}

} // namespace loki::model
