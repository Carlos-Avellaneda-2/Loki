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

const StringParameterDef* EffectDef::stringParameter(std::string_view key) const noexcept {
    for (const auto& p : strings) {
        if (p.key == key) {
            return &p;
        }
    }
    return nullptr;
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
        opacity.masks = true;
        r.push_back(std::move(opacity));

        EffectDef volume;
        volume.type = std::string(kVolumeEffect);
        volume.label = "Volume";
        volume.fixed = true;
        volume.kind = TrackKind::Audio;
        volume.parameters = {
            {"level", "Level", 0, DefaultKind::Constant, kSilenceDb, 15, 0.5, 1, "dB", {}, true},
        };
        r.push_back(std::move(volume));

        EffectDef panner;
        panner.type = std::string(kPannerEffect);
        panner.label = "Panner";
        panner.fixed = true;
        panner.kind = TrackKind::Audio;
        panner.parameters = {
            {"pan", "Pan", 0, DefaultKind::Constant, -100, 100, 1, 1, "", {}, true},
        };
        r.push_back(std::move(panner));

        EffectDef text;
        text.type = std::string(kTextEffect);
        text.label = "Text";
        text.fixed = true;
        text.generator = std::string(kTitleGenerator);
        text.parameters = {
            {"size", "Font Size", 96, DefaultKind::Constant, 4, 1000, 1, 0, "px", {}, true},
            {"positionX", "Position X", 0, DefaultKind::SequenceCenterX, -100000, 100000, 1, 1, "", {}, true},
            {"positionY", "Position Y", 0, DefaultKind::SequenceCenterY, -100000, 100000, 1, 1, "", {}, true},
            {"shadow", "Shadow", 1, DefaultKind::Constant, 0, 1, 1, 0, "", {"Off", "On"}, false},
            {"shadowDistance", "Shadow Distance", 6, DefaultKind::Constant, 0, 200, 1, 1, "px", {}, true},
            {"shadowOpacity", "Shadow Opacity", 60, DefaultKind::Constant, 0, 100, 1, 1, "%", {}, true},
        };
        text.strings = {
            {"text", "Text", "Title", StringKind::Text},
            {"font", "Font", "Arial", StringKind::Font},
            {"color", "Color", "#ffffff", StringKind::Color},
        };
        r.push_back(std::move(text));

        // Regular (non-fixed) effects, added from the Effects panel.
        EffectDef color;
        color.type = std::string(kColorEffect);
        color.label = "Color Correction";
        color.parameters = {
            {"exposure", "Exposure", 0, DefaultKind::Constant, -4, 4, 0.1, 2, "EV", {}, true},
            {"contrast", "Contrast", 0, DefaultKind::Constant, -100, 100, 1, 1, "", {}, true},
            {"saturation", "Saturation", 100, DefaultKind::Constant, 0, 200, 1, 1, "%", {}, true},
            {"temperature", "Temperature", 0, DefaultKind::Constant, -100, 100, 1, 1, "", {}, true},
        };
        color.masks = true;
        r.push_back(std::move(color));

        EffectDef lut;
        lut.type = std::string(kLutEffect);
        lut.label = "LUT";
        lut.parameters = {
            {"intensity", "Intensity", 100, DefaultKind::Constant, 0, 100, 1, 1, "%", {}, true},
        };
        lut.strings = {{"file", "LUT File (.cube)", "", StringKind::File}};
        lut.masks = true;
        r.push_back(std::move(lut));

        const std::vector<std::string> offOn{"Off", "On"};
        EffectDef lumetri;
        lumetri.type = std::string(kLumetriEffect);
        lumetri.label = "Lumetri Color";
        lumetri.masks = true;
        lumetri.curves = true;
        for (const auto& [range, label] : {std::pair{"shadows", "Shadows"}, std::pair{"midtones", "Midtones"},
                                           std::pair{"highlights", "Highlights"}}) {
            const std::string k(range);
            const std::string l(label);
            lumetri.parameters.push_back({k + "Hue", l + " Hue", 0, DefaultKind::Constant, 0, 360, 1, 0, "\xC2\xB0", {}, true});
            lumetri.parameters.push_back({k + "Amount", l + " Color", 0, DefaultKind::Constant, 0, 100, 1, 1, "%", {}, true});
            lumetri.parameters.push_back({k + "Luma", l + " Brightness", 0, DefaultKind::Constant, -100, 100, 1, 1, "", {}, true});
        }
        lumetri.parameters.push_back({"hslEnabled", "HSL Secondary", 0, DefaultKind::Constant, 0, 1, 1, 0, "", offOn, false});
        lumetri.parameters.push_back({"hslHue", "HSL Hue", 120, DefaultKind::Constant, 0, 360, 1, 0, "\xC2\xB0", {}, true});
        lumetri.parameters.push_back({"hslRange", "HSL Hue Range", 30, DefaultKind::Constant, 0, 180, 1, 0, "\xC2\xB0", {}, true});
        lumetri.parameters.push_back({"hslSoftness", "HSL Softness", 20, DefaultKind::Constant, 0, 100, 1, 0, "", {}, true});
        lumetri.parameters.push_back({"hslSatMin", "HSL Min Saturation", 10, DefaultKind::Constant, 0, 100, 1, 0, "%", {}, true});
        lumetri.parameters.push_back({"hslLumaMin", "HSL Min Luma", 0, DefaultKind::Constant, 0, 100, 1, 0, "%", {}, true});
        lumetri.parameters.push_back({"hslLumaMax", "HSL Max Luma", 100, DefaultKind::Constant, 0, 100, 1, 0, "%", {}, true});
        lumetri.parameters.push_back({"hslHueShift", "HSL Hue Shift", 0, DefaultKind::Constant, -180, 180, 1, 0, "\xC2\xB0", {}, true});
        lumetri.parameters.push_back({"hslSaturation", "HSL Saturation", 100, DefaultKind::Constant, 0, 200, 1, 0, "%", {}, true});
        lumetri.parameters.push_back({"hslLuma", "HSL Brightness", 0, DefaultKind::Constant, -100, 100, 1, 0, "", {}, true});
        lumetri.parameters.push_back({"hslShowMask", "Show Mask", 0, DefaultKind::Constant, 0, 1, 1, 0, "", offOn, false});
        lumetri.parameters.push_back({"vignetteAmount", "Vignette Amount", 0, DefaultKind::Constant, -100, 100, 1, 1, "", {}, true});
        lumetri.parameters.push_back({"vignetteMidpoint", "Vignette Midpoint", 50, DefaultKind::Constant, 0, 100, 1, 1, "", {}, true});
        lumetri.parameters.push_back({"vignetteFeather", "Vignette Feather", 50, DefaultKind::Constant, 0, 100, 1, 1, "", {}, true});
        r.push_back(std::move(lumetri));

        EffectDef key;
        key.type = std::string(kUltraKeyEffect);
        key.label = "Ultra Key";
        key.parameters = {
            {"tolerance", "Tolerance", 30, DefaultKind::Constant, 0, 100, 1, 1, "", {}, true},
            {"softness", "Softness", 15, DefaultKind::Constant, 0, 100, 1, 1, "", {}, true},
            {"choke", "Choke", 0, DefaultKind::Constant, 0, 100, 1, 1, "", {}, true},
            {"spill", "Spill Suppression", 50, DefaultKind::Constant, 0, 100, 1, 1, "", {}, true},
            {"output", "Output", 0, DefaultKind::Constant, 0, 1, 1, 0, "", {"Composite", "Alpha Channel"}, false},
        };
        key.strings = {{"keyColor", "Key Color", "#00ff00", StringKind::Color}};
        r.push_back(std::move(key));

        EffectDef blur;
        blur.type = std::string(kGaussianBlurEffect);
        blur.label = "Gaussian Blur";
        blur.masks = true;
        blur.parameters = {
            {"blurriness", "Blurriness", 10, DefaultKind::Constant, 0, 500, 1, 1, "px", {}, true},
            {"dimensions", "Blur Dimensions", 0, DefaultKind::Constant, 0, 2, 1, 0, "",
             {"Horizontal and Vertical", "Horizontal", "Vertical"}, false},
        };
        r.push_back(std::move(blur));

        EffectDef sharpen;
        sharpen.type = std::string(kSharpenEffect);
        sharpen.label = "Sharpen";
        sharpen.masks = true;
        sharpen.parameters = {
            {"amount", "Sharpen Amount", 50, DefaultKind::Constant, 0, 500, 1, 0, "", {}, true},
        };
        r.push_back(std::move(sharpen));

        EffectDef stabilizer;
        stabilizer.type = std::string(kStabilizerEffect);
        stabilizer.label = "Warp Stabilizer";
        stabilizer.analysis = true;
        stabilizer.parameters = {
            {"smoothness", "Smoothness", 50, DefaultKind::Constant, 0, 100, 1, 0, "%", {}, false},
            {"method", "Method", 1, DefaultKind::Constant, 0, 1, 1, 0, "", {"Position", "Position, Scale, Rotation"}, false},
            {"framing", "Framing", 1, DefaultKind::Constant, 0, 1, 1, 0, "",
             {"Stabilize Only", "Stabilize, Crop, Auto-scale"}, false},
        };
        r.push_back(std::move(stabilizer));

        EffectDef eq;
        eq.type = std::string(kEqEffect);
        eq.label = "Parametric Equalizer";
        eq.kind = TrackKind::Audio;
        eq.parameters = {
            {"lowGain", "Low Gain", 0, DefaultKind::Constant, -24, 24, 0.5, 1, "dB", {}, true},
            {"lowFreq", "Low Frequency", 100, DefaultKind::Constant, 20, 1000, 10, 0, "Hz", {}, false},
            {"midGain", "Mid Gain", 0, DefaultKind::Constant, -24, 24, 0.5, 1, "dB", {}, true},
            {"midFreq", "Mid Frequency", 1000, DefaultKind::Constant, 100, 10000, 10, 0, "Hz", {}, false},
            {"midQ", "Mid Q", 1, DefaultKind::Constant, 0.1, 10, 0.1, 2, "", {}, false},
            {"highGain", "High Gain", 0, DefaultKind::Constant, -24, 24, 0.5, 1, "dB", {}, true},
            {"highFreq", "High Frequency", 8000, DefaultKind::Constant, 1000, 20000, 100, 0, "Hz", {}, false},
        };
        r.push_back(std::move(eq));

        EffectDef compressor;
        compressor.type = std::string(kCompressorEffect);
        compressor.label = "Dynamics (Compressor)";
        compressor.kind = TrackKind::Audio;
        compressor.parameters = {
            {"threshold", "Threshold", -20, DefaultKind::Constant, -60, 0, 0.5, 1, "dB", {}, false},
            {"ratio", "Ratio", 4, DefaultKind::Constant, 1, 30, 0.5, 1, ":1", {}, false},
            {"attack", "Attack", 10, DefaultKind::Constant, 0.1, 200, 1, 1, "ms", {}, false},
            {"release", "Release", 120, DefaultKind::Constant, 5, 2000, 5, 0, "ms", {}, false},
            {"makeup", "Make-up Gain", 0, DefaultKind::Constant, 0, 30, 0.5, 1, "dB", {}, false},
        };
        r.push_back(std::move(compressor));

        EffectDef noise;
        noise.type = std::string(kNoiseReductionEffect);
        noise.label = "Noise Reduction";
        noise.kind = TrackKind::Audio;
        noise.parameters = {
            {"reduction", "Reduction", 12, DefaultKind::Constant, 0, 40, 1, 0, "dB", {}, false},
        };
        r.push_back(std::move(noise));
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
        for (const auto& s : def->strings) {
            e.strings[s.key] = s.defaultValue;
        }
    }
    return e;
}

std::string effectiveString(const Clip& clip, std::string_view effectType, std::string_view key) {
    const std::string k(key);
    if (const auto* effect = findEffectByType(clip, effectType); effect != nullptr && effect->enabled) {
        if (const auto it = effect->strings.find(k); it != effect->strings.end()) {
            return it->second;
        }
    }
    const auto* def = findEffectDef(effectType);
    const auto* sdef = def != nullptr ? def->stringParameter(key) : nullptr;
    return sdef != nullptr ? sdef->defaultValue : std::string();
}

bool fixedEffectApplies(const EffectDef& def, TrackKind kind, std::string_view generator) noexcept {
    return def.fixed && def.kind == kind && (def.generator.empty() || def.generator == generator);
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

ClipAudioMix resolveAudioMix(const Clip& clip, const Rational& sourceTime) {
    // Audio parameters have constant defaults, so no media/settings are needed.
    static const MediaInfo kNoMedia;
    static const SequenceSettings kNoSettings;
    ClipAudioMix m;
    m.levelDb = std::clamp(effectiveValue(clip, kVolumeEffect, "level", sourceTime, kNoMedia, kNoSettings), kSilenceDb,
                           15.0);
    m.pan = std::clamp(effectiveValue(clip, kPannerEffect, "pan", sourceTime, kNoMedia, kNoSettings), -100.0, 100.0);
    return m;
}

} // namespace loki::model
