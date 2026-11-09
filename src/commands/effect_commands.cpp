#include "commands/effect_commands.h"

#include <algorithm>
#include <string>

namespace loki::commands {

using namespace loki::model;

ClipContext clipContext(const Project& project, ClipId clip) {
    const auto ref = project.findClip(clip);
    if (ref.clip == nullptr) {
        throw CommandError("Clip does not exist");
    }
    ClipContext ctx;
    ctx.clip = ref.clip;
    ctx.settings = ref.sequence->settings;
    if (const auto* media = project.findMedia(ref.clip->media)) {
        ctx.media = media->info;
    }
    return ctx;
}

namespace {

// Validates the effect/parameter names and returns everything an edit lambda needs.
struct Target {
    std::string type;
    std::string key;
    MediaInfo media;
    SequenceSettings settings;
};

Target target(const Project& project, ClipId clip, std::string_view type, std::string_view key) {
    const auto ctx = clipContext(project, clip);
    const auto* def = findEffectDef(type);
    if (def == nullptr) {
        throw CommandError("Unknown effect " + std::string(type));
    }
    if (!key.empty() && def->parameter(key) == nullptr) {
        throw CommandError("Unknown parameter " + std::string(key));
    }
    return {std::string(type), std::string(key), ctx.media, ctx.settings};
}

Effect& ensureEffect(Clip& c, const Target& t) {
    for (auto& e : c.effects) {
        if (e.type == t.type) {
            return e;
        }
    }
    c.effects.push_back(makeEffect(t.type, t.media, t.settings));
    return c.effects.back();
}

Parameter& ensureParameter(Effect& effect, const Target& t) {
    for (auto& p : effect.parameters) {
        if (p.key == t.key) {
            return p;
        }
    }
    const auto* def = findEffectDef(t.type)->parameter(t.key);
    effect.parameters.push_back({t.key, defaultValue(*def, t.media, t.settings), {}});
    return effect.parameters.back();
}

void putKeyframe(std::vector<Keyframe>& keys, const Rational& time, double value) {
    const auto it = std::lower_bound(keys.begin(), keys.end(), time,
                                     [](const Keyframe& k, const Rational& t) { return k.time < t; });
    if (it != keys.end() && it->time == time) {
        it->value = value;
    } else {
        Keyframe k;
        k.time = time;
        k.value = value;
        // New keyframes take the interpolation of the previous one (Premiere behaviour).
        if (it != keys.begin()) {
            k.interpolation = std::prev(it)->interpolation;
        }
        keys.insert(it, k);
    }
}

std::string gestureKey(GestureId gesture, const Target& t) {
    return gesture == 0 ? std::string() : std::to_string(gesture) + ":fx:" + t.type + ":" + t.key;
}

} // namespace

std::unique_ptr<Command> setEffectString(const Project& project, ClipId clip, std::string_view effectType,
                                         std::string_view key, std::string value) {
    const auto t = target(project, clip, effectType, {});
    const auto* sdef = findEffectDef(effectType)->stringParameter(key);
    if (sdef == nullptr) {
        throw CommandError("Unknown text parameter " + std::string(key));
    }
    return std::make_unique<EditClipCommand>(clip, "Change " + sdef->label,
                                             [t, k = std::string(key), value = std::move(value)](Clip& c) {
                                                 ensureEffect(c, t).strings[k] = value;
                                             });
}

std::unique_ptr<Command> applyEffect(const Project& project, ClipId clip, std::string_view effectType) {
    const auto t = target(project, clip, effectType, {});
    const auto ref = project.findClip(clip);
    const auto* def = findEffectDef(effectType);
    if (def->fixed) {
        throw CommandError(def->label + " is always available in Effect Controls");
    }
    if (def->kind != ref.track->kind) {
        throw CommandError(def->label + (def->kind == TrackKind::Video ? " needs a video clip" : " needs an audio clip"));
    }
    if (findEffectByType(*ref.clip, effectType) != nullptr) {
        throw CommandError("The clip already has " + def->label);
    }
    return std::make_unique<EditClipCommand>(clip, "Apply " + def->label, [t](Clip& c) { (void)ensureEffect(c, t); });
}

std::unique_ptr<Command> setEffectParameter(const Project& project, ClipId clip, std::string_view effectType,
                                            std::string_view key, double value, const Rational& sourceTime,
                                            GestureId gesture) {
    const auto t = target(project, clip, effectType, key);
    const auto* def = findEffectDef(effectType)->parameter(key);
    const double clamped = std::clamp(value, def->minimum, def->maximum);
    return std::make_unique<EditClipCommand>(
        clip, "Change " + def->label,
        [t, clamped, sourceTime](Clip& c) {
            auto& p = ensureParameter(ensureEffect(c, t), t);
            if (p.isAnimated()) {
                putKeyframe(p.keyframes, sourceTime, clamped);
            } else {
                p.value = clamped;
            }
        },
        gestureKey(gesture, t));
}

std::unique_ptr<Command> setParameterAnimated(const Project& project, ClipId clip, std::string_view effectType,
                                              std::string_view key, bool on, const Rational& sourceTime) {
    const auto t = target(project, clip, effectType, key);
    const auto ctx = clipContext(project, clip);
    const double current = effectiveValue(*ctx.clip, effectType, key, sourceTime, t.media, t.settings);
    return std::make_unique<EditClipCommand>(clip, on ? "Animate" : "Stop Animating", [t, on, current, sourceTime](Clip& c) {
        auto& p = ensureParameter(ensureEffect(c, t), t);
        p.keyframes.clear();
        if (on) {
            putKeyframe(p.keyframes, sourceTime, current);
        } else {
            p.value = current;
        }
    });
}

std::unique_ptr<Command> toggleKeyframe(const Project& project, ClipId clip, std::string_view effectType,
                                        std::string_view key, const Rational& sourceTime) {
    const auto t = target(project, clip, effectType, key);
    const auto ctx = clipContext(project, clip);
    const double current = effectiveValue(*ctx.clip, effectType, key, sourceTime, t.media, t.settings);
    return std::make_unique<EditClipCommand>(clip, "Toggle Keyframe", [t, current, sourceTime](Clip& c) {
        auto& p = ensureParameter(ensureEffect(c, t), t);
        const auto before = p.keyframes.size();
        std::erase_if(p.keyframes, [&](const Keyframe& k) { return k.time == sourceTime; });
        if (p.keyframes.size() == before) {
            if (!p.isAnimated()) {
                p.value = current;
            }
            putKeyframe(p.keyframes, sourceTime, current);
        } else if (p.keyframes.empty()) {
            p.value = current; // removing the last keyframe keeps the value
        }
    });
}

std::unique_ptr<Command> moveKeyframe(const Project& project, ClipId clip, std::string_view effectType,
                                      std::string_view key, const Rational& from, const Rational& to,
                                      GestureId gesture) {
    const auto t = target(project, clip, effectType, key);
    if (to.isNegative()) {
        throw CommandError("Keyframes cannot move before the start of the media");
    }
    return std::make_unique<EditClipCommand>(
        clip, "Move Keyframe",
        [t, from, to](Clip& c) {
            auto& p = ensureParameter(ensureEffect(c, t), t);
            const auto it = std::find_if(p.keyframes.begin(), p.keyframes.end(), [&](const Keyframe& k) { return k.time == from; });
            if (it == p.keyframes.end()) {
                throw CommandError("No keyframe at that time");
            }
            if (from == to) {
                return;
            }
            if (std::any_of(p.keyframes.begin(), p.keyframes.end(), [&](const Keyframe& k) { return k.time == to; })) {
                throw CommandError("There is already a keyframe there");
            }
            it->time = to;
            std::sort(p.keyframes.begin(), p.keyframes.end(),
                      [](const Keyframe& a, const Keyframe& b) { return a.time < b.time; });
        },
        gesture == 0 ? std::string() : std::to_string(gesture) + ":kfmove:" + t.type + ":" + t.key);
}

std::unique_ptr<Command> setKeyframeInterpolation(const Project& project, ClipId clip, std::string_view effectType,
                                                  std::string_view key, const Rational& time,
                                                  Interpolation interpolation) {
    const auto t = target(project, clip, effectType, key);
    return std::make_unique<EditClipCommand>(clip, "Keyframe Interpolation", [t, time, interpolation](Clip& c) {
        auto& p = ensureParameter(ensureEffect(c, t), t);
        const auto it = std::find_if(p.keyframes.begin(), p.keyframes.end(), [&](const Keyframe& k) { return k.time == time; });
        if (it == p.keyframes.end()) {
            throw CommandError("No keyframe at that time");
        }
        it->interpolation = interpolation;
    });
}

std::unique_ptr<Command> resetEffect(const Project& project, ClipId clip, std::string_view effectType) {
    const auto t = target(project, clip, effectType, {});
    return std::make_unique<EditClipCommand>(clip, "Reset Effect", [t](Clip& c) {
        std::erase_if(c.effects, [&](const Effect& e) { return e.type == t.type; });
    });
}

} // namespace loki::commands
