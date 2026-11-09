#pragma once

#include "model/ids.h"
#include "model/keyframe.h"
#include "util/rational.h"

#include <cstdint>
#include <filesystem>
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

struct MediaItem {
    MediaId id;
    std::string name;
    std::filesystem::path path; // absolute path of the original file
    std::optional<std::filesystem::path> proxyPath;
    BinId bin; // null = project root
    MediaInfo info;
    // Source Monitor In/Out marks (source time). Used by insert/overwrite.
    std::optional<Rational> markIn;
    std::optional<Rational> markOut;

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

    [[nodiscard]] Rational end() const { return start + duration; }
    [[nodiscard]] Rational sourceOut() const { return sourceIn + duration; }
    // Converts a sequence time inside this clip to its source time.
    [[nodiscard]] Rational toSourceTime(const Rational& sequenceTime) const { return sequenceTime - start + sourceIn; }
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

struct TrackState {
    std::string name;
    bool locked = false;
    bool hidden = false; // video: output disabled (eye)
    bool muted = false;  // audio: muted
    // Audio mixer (D16): fader in dB, pan -100 (L) .. 100 (R), solo.
    double volumeDb = 0.0;
    double pan = 0.0;
    bool solo = false;

    friend bool operator==(const TrackState&, const TrackState&) = default;
};

struct Track {
    TrackId id;
    TrackKind kind = TrackKind::Video;
    TrackState state;
    std::vector<Clip> clips; // sorted by start, never overlapping

    [[nodiscard]] Rational end() const { return clips.empty() ? Rational() : clips.back().end(); }

    friend bool operator==(const Track&, const Track&) = default;
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
