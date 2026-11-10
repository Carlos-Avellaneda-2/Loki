#pragma once

#include "model/ids.h"
#include "model/keyframe.h"
#include "model/mask.h"
#include "util/rational.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace loki::model {

// Plain value types. They are only reachable through `const` references from Project, so the
// public fields cannot be changed outside ProjectMutator (and therefore outside Commands).

struct MediaInfo {
    bool hasVideo = false;
    bool hasAudio = false;
    int width = 0;
    int height = 0;
    Rational frameRate;     // 0 when unknown / audio-only
    Rational duration;      // 0 when unknown
    int sampleRate = 0;
    int audioChannels = 0;
    std::string videoCodec;
    std::string audioCodec;
    bool isStill = false; // single image: no intrinsic duration

    friend bool operator==(const MediaInfo&, const MediaInfo&) = default;
};

// Generated media (D20): no file; the renderer synthesizes the picture.
inline constexpr std::string_view kTitleGenerator = "loki.title";
// Nested sequence (D30): the media shows another sequence of the project (MediaItem::sequence).
inline constexpr std::string_view kNestGenerator = "loki.nest";
// Adjustment layer (D31): its color effects apply to the composite of the tracks below.
inline constexpr std::string_view kAdjustmentGenerator = "loki.adjustment";

// Fastest clip speed (10000 %).
inline constexpr int kMaxClipSpeed = 100;

// Marker colors (D29), Premiere's order.
enum class MarkerColor { Green = 0, Red, Purple, Orange, Yellow, White, Blue, Cyan };
inline constexpr int kMarkerColorCount = 8;

// A sequence marker (sequence time) or a clip marker (source time of a media item) (D29).
struct Marker {
    MarkerId id;
    Rational time;
    Rational duration; // 0 = a point marker
    std::string name;
    std::string comment;
    MarkerColor color = MarkerColor::Green;

    friend bool operator==(const Marker&, const Marker&) = default;
};

struct MediaItem {
    MediaId id;
    std::string name;
    std::filesystem::path path; // absolute path of the original file (empty for generated media)
    std::string generator;      // non-empty: generated media (kTitleGenerator)
    std::optional<std::filesystem::path> proxyPath;
    BinId bin; // null = project root
    MediaInfo info;
    // Source Monitor In/Out marks (source time). Used by insert/overwrite.
    std::optional<Rational> markIn;
    std::optional<Rational> markOut;
    SequenceId sequence;         // kNestGenerator: the nested sequence (D30)
    bool multicam = false;       // kNestGenerator: a multi-camera source (D43), clips pick an angle
    std::vector<Marker> markers; // clip markers, source time, sorted by time (D29)

    [[nodiscard]] bool isGenerated() const noexcept { return !generator.empty(); }
    [[nodiscard]] bool isNest() const noexcept { return generator == kNestGenerator; }
    [[nodiscard]] bool isAdjustment() const noexcept { return generator == kAdjustmentGenerator; }

    friend bool operator==(const MediaItem&, const MediaItem&) = default;
};

struct Bin {
    BinId id;
    std::string name;
    BinId parent; // null = project root

    friend bool operator==(const Bin&, const Bin&) = default;
};

struct Effect {
    EffectId id;
    std::string type; // e.g. "loki.motion", "loki.opacity"
    bool enabled = true;
    std::vector<Parameter> parameters;
    // Non-animatable text parameters (title text, font, LUT file) (D20).
    std::map<std::string, std::string> strings;
    // Phase 14 (format v4): masks limiting where the effect applies (D34), color curves by name
    // ("master", "red", "green", "blue", D33) and analysis data (stabilizer, D36).
    std::vector<Mask> masks;
    std::map<std::string, std::vector<CurvePoint>> curves;
    std::vector<double> samples;

    [[nodiscard]] const Mask* findMask(MaskId maskId) const noexcept {
        for (const auto& m : masks) {
            if (m.id == maskId) {
                return &m;
            }
        }
        return nullptr;
    }

    [[nodiscard]] std::string string(const std::string& key, const std::string& fallback = {}) const {
        const auto it = strings.find(key);
        return it != strings.end() ? it->second : fallback;
    }

    [[nodiscard]] const Parameter* findParameter(std::string_view key) const noexcept {
        for (const auto& p : parameters) {
            if (p.key == key) {
                return &p;
            }
        }
        return nullptr;
    }

    friend bool operator==(const Effect&, const Effect&) = default;
};

struct Clip {
    ClipId id;
    MediaId media;
    std::string name;
    Rational start;    // position on the sequence timeline
    Rational sourceIn; // first source time used
    Rational duration; // > 0
    bool enabled = true;
    LinkId link; // clips sharing a non-null link move together (A/V link, Phase 7)
    // Audio fade handles (D17): lengths from the clip's start / end; 0 = none. Clamped to the
    // clip duration when mixing, so trims never make them invalid.
    Rational fadeIn;
    Rational fadeOut;
    std::vector<Effect> effects;
    // Speed (D28): source seconds per sequence second (1 = 100 %, 1/2 = 50 % slow motion). 0 holds
    // the frame at sourceIn (Frame Hold). `reverse` plays [sourceIn, sourceOut) backwards.
    Rational speed{1};
    bool reverse = false;
    bool maintainPitch = false; // D38: retimed audio keeps its pitch (time stretch)
    int angle = 0;              // D43: multicam clips show this video track (0 = V1) of the source

    [[nodiscard]] Rational end() const { return start + duration; }
    // Source time the clip covers: [sourceIn, sourceOut()).
    [[nodiscard]] Rational sourceSpan() const { return duration * speed; }
    [[nodiscard]] Rational sourceOut() const { return sourceIn + sourceSpan(); }
    [[nodiscard]] bool isRetimed() const { return speed != Rational(1) || reverse; }
    // Converts a sequence time inside this clip to its source time.
    [[nodiscard]] Rational toSourceTime(const Rational& sequenceTime) const {
        const Rational offset = (sequenceTime - start) * speed;
        return reverse ? sourceOut() - offset : sourceIn + offset;
    }
    // Inverse of toSourceTime (the clip start for a frame hold).
    [[nodiscard]] Rational toSequenceTime(const Rational& sourceTime) const {
        if (speed.isZero()) {
            return start;
        }
        return start + (reverse ? sourceOut() - sourceTime : sourceTime - sourceIn) / speed;
    }
    // Source time moved per sequence time when slipping (a frame hold slips 1:1).
    [[nodiscard]] Rational slipFactor() const { return speed.isZero() ? Rational(1) : speed; }
    // Moves the In edge by `delta` sequence time (positive = later), keeping the content in place.
    void moveIn(const Rational& delta) {
        start += delta;
        duration -= delta;
        if (!reverse) {
            sourceIn += delta * speed;
        }
    }
    // Moves the Out edge by `delta` sequence time (positive = later), keeping the content in place.
    void moveOut(const Rational& delta) {
        duration += delta;
        if (reverse) {
            sourceIn -= delta * speed;
        }
    }
    // How far (sequence time, >= 0) the In edge can move earlier / the Out edge later before the
    // source runs out. `limit` is the media duration (nullopt = unlimited). nullopt = unlimited.
    [[nodiscard]] std::optional<Rational> roomBefore(const std::optional<Rational>& limit) const {
        if (speed.isZero()) {
            return std::nullopt;
        }
        if (!reverse) {
            return sourceIn / speed;
        }
        return limit ? std::optional<Rational>(max(Rational(), *limit - sourceOut()) / speed) : std::nullopt;
    }
    [[nodiscard]] std::optional<Rational> roomAfter(const std::optional<Rational>& limit) const {
        if (speed.isZero()) {
            return std::nullopt;
        }
        if (reverse) {
            return sourceIn / speed;
        }
        return limit ? std::optional<Rational>(max(Rational(), *limit - sourceOut()) / speed) : std::nullopt;
    }
    [[nodiscard]] const Effect* findEffect(EffectId effectId) const noexcept {
        for (const auto& e : effects) {
            if (e.id == effectId) {
                return &e;
            }
        }
        return nullptr;
    }

    friend bool operator==(const Clip&, const Clip&) = default;
};

enum class TrackKind { Video, Audio };

// Where a two-sided transition sits relative to the cut (D19).
enum class TransitionAlignment { Center, StartAtCut, EndAtCut };

// A transition on a track (D19). Two-sided: between `from` (left) and `to` (right) clips that touch
// at a cut. One-sided: only `from` (at its end) or only `to` (at its start). A transition whose
// clips no longer exist or no longer touch is ignored by the renderer (edits remove it).
struct Transition {
    TransitionId id;
    std::string type; // "loki.crossDissolve", "loki.dipToBlack", "loki.wipe", "loki.crossfade"
    ClipId from;
    ClipId to;
    Rational duration; // > 0
    TransitionAlignment alignment = TransitionAlignment::Center;

    friend bool operator==(const Transition&, const Transition&) = default;
};

struct TrackState {
    std::string name;
    bool locked = false;
    bool hidden = false; // video: output disabled (eye)
    bool muted = false;  // audio: muted
    // Audio mixer (D16): fader in dB, pan -100 (L) .. 100 (R), solo.
    double volumeDb = 0.0;
    double pan = 0.0;
    bool solo = false;
    // Track automation (D39): keyframes in *sequence* time; when present they replace the static
    // fader (volumeDb) / pan.
    std::vector<Keyframe> volumeAutomation;
    std::vector<Keyframe> panAutomation;

    [[nodiscard]] double volumeAt(const Rational& t) const {
        return volumeAutomation.empty() ? volumeDb : Parameter{"", volumeDb, volumeAutomation}.valueAt(t);
    }
    [[nodiscard]] double panAt(const Rational& t) const {
        return panAutomation.empty() ? pan : Parameter{"", pan, panAutomation}.valueAt(t);
    }

    friend bool operator==(const TrackState&, const TrackState&) = default;
};

struct Track {
    TrackId id;
    TrackKind kind = TrackKind::Video;
    TrackState state;
    std::vector<Clip> clips; // sorted by start, never overlapping
    std::vector<Transition> transitions;

    [[nodiscard]] const Clip* findClip(ClipId clipId) const noexcept {
        for (const auto& c : clips) {
            if (c.id == clipId) {
                return &c;
            }
        }
        return nullptr;
    }
    [[nodiscard]] const Transition* findTransition(TransitionId transitionId) const noexcept {
        for (const auto& t : transitions) {
            if (t.id == transitionId) {
                return &t;
            }
        }
        return nullptr;
    }

    [[nodiscard]] Rational end() const { return clips.empty() ? Rational() : clips.back().end(); }

    friend bool operator==(const Track&, const Track&) = default;
};

// A caption (D40), sequence time.
struct Caption {
    CaptionId id;
    Rational start;
    Rational duration; // > 0
    std::string text;

    [[nodiscard]] Rational end() const { return start + duration; }
    friend bool operator==(const Caption&, const Caption&) = default;
};

// How the captions of a sequence look (one style per sequence).
struct CaptionStyle {
    std::string font = "Arial";
    double size = 54.0;           // px at the sequence size
    std::string color = "#ffffff";
    double backgroundOpacity = 60.0; // % of a black box behind the text
    double positionY = 88.0;      // % from the top of the frame (text baseline area)
    bool visible = true;          // shown in the monitors (export decides burn-in separately)

    friend bool operator==(const CaptionStyle&, const CaptionStyle&) = default;
};

struct SequenceSettings {
    int width = 1920;
    int height = 1080;
    Rational frameRate{30, 1};
    int sampleRate = 48000;

    friend bool operator==(const SequenceSettings&, const SequenceSettings&) = default;
};

struct Sequence {
    SequenceId id;
    std::string name;
    SequenceSettings settings;
    std::vector<Track> videoTracks; // index 0 = V1 (bottom)
    std::vector<Track> audioTracks; // index 0 = A1
    // Sequence In/Out marks (lift/extract, D13); Out is exclusive.
    std::optional<Rational> markIn;
    std::optional<Rational> markOut;
    double masterVolumeDb = 0.0; // master fader (D16)
    std::vector<Marker> markers; // sequence markers, sorted by time (D29)
    std::vector<Caption> captions; // sorted by start, not overlapping (D40)
    CaptionStyle captionStyle;

    [[nodiscard]] const std::vector<Track>& tracks(TrackKind kind) const noexcept {
        return kind == TrackKind::Video ? videoTracks : audioTracks;
    }
    [[nodiscard]] Rational duration() const {
        Rational d;
        for (const auto* list : {&videoTracks, &audioTracks}) {
            for (const auto& t : *list) {
                d = max(d, t.end());
            }
        }
        return d;
    }

    friend bool operator==(const Sequence&, const Sequence&) = default;
};

} // namespace loki::model
