#include "model/effect_registry.h"

#include <algorithm>
#include <cmath>

namespace loki::model {

const char* blendModeName(BlendMode mode) noexcept {
    switch (mode) {
    case BlendMode::Normal: return "Normal";
    case BlendMode::Multiply: return "Multiply";
    case BlendMode::Screen: return "Screen";
    case BlendMode::Overlay: return "Overlay";
    case BlendMode::Add: return "Add";
    case BlendMode::Darken: return "Darken";
    case BlendMode::Lighten: return "Lighten";
    case BlendMode::Difference: return "Difference";
    }
    return "Normal";
}

const ParameterDef* EffectDef::parameter(std::string_view key) const noexcept {
    for (const auto& p : parameters) {
        if (p.key == key) {
            return &p;
        }
    }
    return nullptr;
}

const std::vector<EffectDef>& effectRegistry() {
    static const std::vector<EffectDef> kRegistry = [] {
        std::vector<EffectDef> r;
        EffectDef motion;
        motion.type = std::string(kMotionEffect);
        motion.label = "Motion";
        motion.fixed = true;
        motion.parameters = {
            {"positionX", "Position X", 0, DefaultKind::SequenceCenterX, -100000, 100000, 1, 1, "", {}, true},
            {"positionY", "Position Y", 0, DefaultKind::SequenceCenterY, -100000, 100000, 1, 1, "", {}, true},
            {"scale", "Scale", 100, DefaultKind::Constant, 0, 10000, 1, 1, "%", {}, true},
            {"rotation", "Rotation", 0, DefaultKind::Constant, -36000, 36000, 1, 1, "\xC2\xB0", {}, true},
            {"anchorX", "Anchor Point X", 0, DefaultKind::MediaCenterX, -100000, 100000, 1, 1, "", {}, true},
            {"anchorY", "Anchor Point Y", 0, DefaultKind::MediaCenterY, -100000, 100000, 1, 1, "", {}, true},
        };
        r.push_back(std::move(motion));

        EffectDef opacity;
        opacity.type = std::string(kOpacityEffect);
        opacity.label = "Opacity";
        opacity.fixed = true;
        std::vector<std::string> modes;
        for (int i = 0; i < kBlendModeCount; ++i) {
            modes.emplace_back(blendModeName(static_cast<BlendMode>(i)));
        }
        opacity.parameters = {
            {"opacity", "Opacity", 100, DefaultKind::Constant, 0, 100, 1, 1, "%", {}, true},
            {"blendMode", "Blend Mode", 0, DefaultKind::Constant, 0, kBlendModeCount - 1, 1, 0, "", modes, false},
        };
        r.push_back(std::move(opacity));
        return r;
    }();
    return kRegistry;
}

const EffectDef* findEffectDef(std::string_view type) noexcept {
    for (const auto& def : effectRegistry()) {
        if (def.type == type) {
            return &def;
        }
    }
    return nullptr;
}

double defaultValue(const ParameterDef& def, const MediaInfo& media, const SequenceSettings& settings) {
    switch (def.defaultKind) {
    case DefaultKind::Constant: return def.defaultValue;
    case DefaultKind::SequenceCenterX: return settings.width / 2.0;
    case DefaultKind::SequenceCenterY: return settings.height / 2.0;
    case DefaultKind::MediaCenterX: return (media.width > 0 ? media.width : settings.width) / 2.0;
    case DefaultKind::MediaCenterY: return (media.height > 0 ? media.height : settings.height) / 2.0;
    }
    return def.defaultValue;
}

const Effect* findEffectByType(const Clip& clip, std::string_view type) noexcept {
    for (const auto& e : clip.effects) {
        if (e.type == type) {
            return &e;
        }
    }
    return nullptr;
}

double effectiveValue(const Clip& clip, std::string_view effectType, std::string_view key, const Rational& sourceTime,
                      const MediaInfo& media, const SequenceSettings& settings) {
    if (const auto* effect = findEffectByType(clip, effectType); effect != nullptr && effect->enabled) {
        if (const auto* p = effect->findParameter(key)) {
            return p->valueAt(sourceTime);
        }
    }
    const auto* def = findEffectDef(effectType);
    const auto* pdef = def != nullptr ? def->parameter(key) : nullptr;
    return pdef != nullptr ? defaultValue(*pdef, media, settings) : 0.0;
}

Effect makeEffect(std::string_view type, const MediaInfo& media, const SequenceSettings& settings) {
    Effect e;
    e.id = EffectId::generate();
    e.type = std::string(type);
    if (const auto* def = findEffectDef(type)) {
        for (const auto& p : def->parameters) {
            e.parameters.push_back({p.key, defaultValue(p, media, settings), {}});
        }
    }
    return e;
}

ClipTransform resolveTransform(const Clip& clip, const Rational& t, const MediaInfo& media,
                               const SequenceSettings& settings) {
    ClipTransform x;
    const auto v = [&](std::string_view type, std::string_view key) {
        return effectiveValue(clip, type, key, t, media, settings);
    };
    x.positionX = v(kMotionEffect, "positionX");
    x.positionY = v(kMotionEffect, "positionY");
    x.scale = v(kMotionEffect, "scale");
    x.rotation = v(kMotionEffect, "rotation");
    x.anchorX = v(kMotionEffect, "anchorX");
    x.anchorY = v(kMotionEffect, "anchorY");
    x.opacity = std::clamp(v(kOpacityEffect, "opacity"), 0.0, 100.0);
    const auto mode = static_cast<int>(std::lround(v(kOpacityEffect, "blendMode")));
    x.blend = static_cast<BlendMode>(std::clamp(mode, 0, kBlendModeCount - 1));
    return x;
}

} // namespace loki::model
