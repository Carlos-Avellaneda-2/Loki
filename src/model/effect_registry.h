#pragma once

#include "model/types.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace loki::model {

// Built-in effect types. Motion and Opacity are "fixed" effects (D12): every clip has them in the
// Effect Controls panel; they are only stored in the project once a value is changed.
inline constexpr std::string_view kMotionEffect = "loki.motion";
inline constexpr std::string_view kOpacityEffect = "loki.opacity";
// Audio fixed effects (D15): clip level in dB and pan/balance (-100 L .. 100 R).
inline constexpr std::string_view kVolumeEffect = "loki.volume";
inline constexpr std::string_view kPannerEffect = "loki.panner";
// Phase 9 (D20): color correction, LUT (.cube) and the title text of generated title clips.
inline constexpr std::string_view kColorEffect = "loki.color";
inline constexpr std::string_view kLutEffect = "loki.lut";
inline constexpr std::string_view kTextEffect = "loki.text";
// Levels at or below this many dB are silence (-inf).
inline constexpr double kSilenceDb = -96.0;

enum class BlendMode { Normal = 0, Multiply, Screen, Overlay, Add, Darken, Lighten, Difference };
inline constexpr int kBlendModeCount = 8;
[[nodiscard]] const char* blendModeName(BlendMode mode) noexcept;

// Where a parameter's default value comes from.
enum class DefaultKind {
    Constant,        // ParameterDef::defaultValue
    SequenceCenterX, // sequence width / 2
    SequenceCenterY, // sequence height / 2
    MediaCenterX,    // media width / 2
    MediaCenterY,    // media height / 2
};

struct ParameterDef {
    std::string key;
    std::string label;
    double defaultValue = 0.0;
    DefaultKind defaultKind = DefaultKind::Constant;
    double minimum = -1e9;
    double maximum = 1e9;
    double step = 1.0;
    int decimals = 1;
    std::string unit;                    // shown after the value ("%", "°")
    std::vector<std::string> enumLabels; // non-empty: the value is an index into this list
    bool animatable = true;
};

// Non-animatable text parameter (stored in Effect::strings).
enum class StringKind { Text, Font, Color, File };
struct StringParameterDef {
    std::string key;
    std::string label;
    std::string defaultValue;
    StringKind kind = StringKind::Text;
};

struct EffectDef {
    std::string type;
    std::string label;
    bool fixed = false; // shown on every clip of `kind` (Motion/Opacity on video, Volume/Panner on audio)
    TrackKind kind = TrackKind::Video;
    // Fixed effects with a generator only appear on clips of that generated media (Text on titles).
    std::string generator;
    std::vector<StringParameterDef> strings;
    [[nodiscard]] const StringParameterDef* stringParameter(std::string_view key) const noexcept;
    std::vector<ParameterDef> parameters;

    [[nodiscard]] const ParameterDef* parameter(std::string_view key) const noexcept;
};

// All known effects, fixed ones first (Motion, Opacity).
[[nodiscard]] const std::vector<EffectDef>& effectRegistry();
[[nodiscard]] const EffectDef* findEffectDef(std::string_view type) noexcept;

// Default value of a parameter for a clip of `media` in a sequence with `settings`.
[[nodiscard]] double defaultValue(const ParameterDef& def, const MediaInfo& media, const SequenceSettings& settings);

// Effective value at `sourceTime`: the stored parameter (static or keyframed) or the default.
[[nodiscard]] double effectiveValue(const Clip& clip, std::string_view effectType, std::string_view key,
                                    const Rational& sourceTime, const MediaInfo& media,
                                    const SequenceSettings& settings);

// Effective text parameter: the stored string or the default.
[[nodiscard]] std::string effectiveString(const Clip& clip, std::string_view effectType, std::string_view key);

// Whether a fixed effect applies to a clip on a track of `kind` whose media has `generator`.
[[nodiscard]] bool fixedEffectApplies(const EffectDef& def, TrackKind kind, std::string_view generator) noexcept;

// The clip's effect of `type` (first one), if stored.
[[nodiscard]] const Effect* findEffectByType(const Clip& clip, std::string_view type) noexcept;

// A new effect of `type` with every parameter set to its default (dynamic defaults resolved).
[[nodiscard]] Effect makeEffect(std::string_view type, const MediaInfo& media, const SequenceSettings& settings);

// Resolved Volume + Panner of an audio clip at a source time.
struct ClipAudioMix {
    double levelDb = 0.0;
    double pan = 0.0; // -100 .. 100
};
[[nodiscard]] ClipAudioMix resolveAudioMix(const Clip& clip, const Rational& sourceTime);

// Resolved Motion + Opacity of a clip at a source time (pixels, percent, degrees as stored).
struct ClipTransform {
    double positionX = 0;
    double positionY = 0;
    double scale = 100; // percent
    double rotation = 0; // degrees, clockwise
    double anchorX = 0;
    double anchorY = 0;
    double opacity = 100; // percent
    BlendMode blend = BlendMode::Normal;
};
[[nodiscard]] ClipTransform resolveTransform(const Clip& clip, const Rational& sourceTime, const MediaInfo& media,
                                             const SequenceSettings& settings);

} // namespace loki::model
