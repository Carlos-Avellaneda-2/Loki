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

struct EffectDef {
    std::string type;
    std::string label;
    bool fixed = false; // shown on every clip (Motion, Opacity)
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

// The clip's effect of `type` (first one), if stored.
[[nodiscard]] const Effect* findEffectByType(const Clip& clip, std::string_view type) noexcept;

// A new effect of `type` with every parameter set to its default (dynamic defaults resolved).
[[nodiscard]] Effect makeEffect(std::string_view type, const MediaInfo& media, const SequenceSettings& settings);

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
