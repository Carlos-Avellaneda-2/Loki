#pragma once

#include "audio/mixer.h"
#include "media/video_frame.h"
#include "model/project.h"
#include "model/transitions.h"
#include "playback/effect_resolve.h"
#include "model/types.h"
#include "render/composite.h"
#include "text/title_renderer.h"

#include <cstdint>
#include <filesystem>
#include <list>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace loki::media {
class AudioDecoder;
class VideoDecoder;
} // namespace loki::media

namespace loki::playback {

// A picture on the playback timeline. `frame` may be null (gap / offline media): shown as black.
struct TimedFrame {
    Rational time;     // timeline time where this picture starts
    Rational duration; // how long it is shown
    media::VideoFramePtr frame;   // single picture (file playback) or the topmost layer (sequences)
    render::CompositePtr composite; // sequences: every layer to composite (null for files)
};

// Video provider for the playback engine. Called only from the engine's media thread.
class VideoSource {
public:
    virtual ~VideoSource() = default;
    VideoSource(const VideoSource&) = delete;
    VideoSource& operator=(const VideoSource&) = delete;
    VideoSource(VideoSource&&) = delete;
    VideoSource& operator=(VideoSource&&) = delete;

    // Picture shown at `t`; nullopt past the end.
    virtual std::optional<TimedFrame> frameAt(const Rational& t) = 0;
    // The next nextFrame() returns the picture at `t`.
    virtual void seek(const Rational& t) = 0;
    // Pictures in timeline order; nullopt at the end.
    virtual std::optional<TimedFrame> nextFrame() = 0;
    // A run of consecutive pictures ending with the one at `t` (reverse playback), ascending.
    virtual std::vector<TimedFrame> framesUpTo(const Rational& t) = 0;
    virtual void setMaxSize(int maxWidth, int maxHeight) = 0;
    [[nodiscard]] virtual std::string hwAccel() const { return {}; }

protected:
    VideoSource() = default;
};

// Post-fader level of one audio track during the last read() block.
struct TrackLevel {
    model::TrackId track;
    audio::StereoLevel level;
};

// Audio provider: interleaved float frames at the device rate/channels.
class AudioSource {
public:
    virtual ~AudioSource() = default;
    AudioSource(const AudioSource&) = delete;
    AudioSource& operator=(const AudioSource&) = delete;
    AudioSource(AudioSource&&) = delete;
    AudioSource& operator=(AudioSource&&) = delete;

    virtual void seek(const Rational& t) = 0;
    // Returns frames written; 0 at the end.
    virtual std::size_t read(float* out, std::size_t frames) = 0;
    // Per-track levels of the last read() (empty for single files).
    [[nodiscard]] virtual std::vector<TrackLevel> trackLevels() const { return {}; }

protected:
    AudioSource() = default;
};

// --- Single media file ------------------------------------------------------------------------

class FileVideoSource final : public VideoSource {
public:
    FileVideoSource(const std::filesystem::path& path, bool allowHardware, int maxWidth, int maxHeight);
    ~FileVideoSource() override;
    std::optional<TimedFrame> frameAt(const Rational& t) override;
    void seek(const Rational& t) override;
    std::optional<TimedFrame> nextFrame() override;
    std::vector<TimedFrame> framesUpTo(const Rational& t) override;
    void setMaxSize(int maxWidth, int maxHeight) override;
    [[nodiscard]] std::string hwAccel() const override;
    [[nodiscard]] Rational frameRate() const;

private:
    std::unique_ptr<media::VideoDecoder> m_decoder;
};

class FileAudioSource final : public AudioSource {
public:
    FileAudioSource(const std::filesystem::path& path, int sampleRate, int channels);
    ~FileAudioSource() override;
    void seek(const Rational& t) override;
    std::size_t read(float* out, std::size_t frames) override;

private:
    std::unique_ptr<media::AudioDecoder> m_decoder;
};

// --- Sequence ---------------------------------------------------------------------------------

// Immutable copy of a sequence plus the media it references. Published by the UI after every edit
// and read by the playback thread (D1: copies are cheap and never shared mutably).
struct SequenceSnapshot {
    model::Sequence sequence;
    std::unordered_map<model::MediaId, model::MediaItem> media;
    std::uint64_t generation = 0; // changes with every edit (frame cache key)
    bool useProxies = false;      // preview only (D24); export always reads the originals
    // Sequences nested in this one (D30), each with the media it uses (and its own nested ones).
    std::unordered_map<model::SequenceId, std::shared_ptr<const SequenceSnapshot>> nested;
};

// File to decode pictures from: the proxy when enabled and present, else the original.
[[nodiscard]] std::filesystem::path videoPathFor(const model::MediaItem& media, bool useProxies);

// Builds the composite of every enabled clip on visible video tracks (V1 at the bottom) with its
// Motion and Opacity resolved at the time (D11). One decoder per clip so the same media used twice
// does not make one decoder seek back and forth.
class SequenceVideoSource final : public VideoSource {
public:
    // maxDecoders: video layers kept open at once; parallelPlayback: also decode layers in parallel
    // during sequential playback (machines with spare cores).
    SequenceVideoSource(std::shared_ptr<const SequenceSnapshot> snapshot, bool allowHardware, int maxWidth,
                        int maxHeight, std::size_t maxDecoders = 8, bool parallelPlayback = false);
    ~SequenceVideoSource() override;
    std::optional<TimedFrame> frameAt(const Rational& t) override;
    void seek(const Rational& t) override;
    std::optional<TimedFrame> nextFrame() override;
    std::vector<TimedFrame> framesUpTo(const Rational& t) override;
    void setMaxSize(int maxWidth, int maxHeight) override;
    // Composite at `t` without parallel prefetch (nested sequences, D30); null past the end.
    render::CompositePtr compositeAt(const Rational& t);
    // Nested sequences composite over transparency instead of black.
    void setTransparent(bool transparent) noexcept { m_transparent = transparent; }

private:
    media::VideoDecoder* decoderFor(const model::Clip& clip, const model::MediaItem& media);
    SequenceVideoSource* nestedSourceFor(const model::Clip& clip, const model::MediaItem& media);
    // Decoded picture of a file clip at `sourceTime`; reversed clips read runs backwards (D28).
    media::VideoFramePtr decodedFrame(const model::Clip& clip, const model::MediaItem& media, const Rational& sourceTime);
    // Picture of `clip` at sequence time `t` with Motion/Opacity, color, LUT resolved. `t` may be
    // outside the clip (transition handles): the source time is clamped (freeze frame, D19).
    std::optional<render::Layer> layerFor(const model::Clip& clip, const Rational& t);
    [[nodiscard]] Rational sourceTimeFor(const model::Clip& clip, const model::MediaItem& media, const Rational& t) const;
    // Decodes the pictures every layer needs at `t` in parallel (one decoder per clip), so a seek
    // costs the slowest layer instead of the sum; layerFor() then finds them ready.
    void prefetch(const Rational& t);
    std::optional<TimedFrame> compose(const Rational& t, bool parallel);
    media::VideoFramePtr titleFrame(const model::Clip& clip, const Rational& sourceTime);
    std::shared_ptr<const render::Lut3D> lutFor(const std::string& path);

    std::shared_ptr<const SequenceSnapshot> m_snapshot;
    bool m_allowHardware;
    int m_maxWidth;
    int m_maxHeight;
    Rational m_frameDuration;
    Rational m_next;
    std::list<std::pair<model::ClipId, std::unique_ptr<media::VideoDecoder>>> m_decoders; // LRU
    std::size_t m_maxDecoders = 8;
    bool m_parallelPlayback = false;
    std::unordered_set<model::MediaId> m_offline;
    struct CachedTitle {
        text::TitleSpec spec;
        media::VideoFramePtr frame;
    };
    std::unordered_map<model::ClipId, CachedTitle> m_titles;
    std::unordered_map<std::string, std::shared_ptr<const render::Lut3D>> m_luts; // null = failed to load
    std::list<std::pair<model::ClipId, std::unique_ptr<SequenceVideoSource>>> m_nested; // LRU
    std::unordered_map<model::ClipId, std::vector<media::VideoFramePtr>> m_reverseRuns;
    EffectResolver m_effects;
    bool m_transparent = false;
};

// Copies a sequence and the media its clips use (null if the sequence does not exist).
[[nodiscard]] std::shared_ptr<SequenceSnapshot> makeSnapshot(const model::Project& project, model::SequenceId id,
                                                             std::uint64_t generation);

// True when `a` and `b` differ at most in audio mix settings (track volume/pan/mute/solo, master,
// clip Volume/Panner effects, fades): playback can then keep its decoders and buffers.
[[nodiscard]] bool sameExceptAudioMix(const SequenceSnapshot& a, const SequenceSnapshot& b);

// Multitrack mixer (Phase 8). Per clip: Volume level (keyframes) x fade handles x Panner (mono
// material: constant-power pan, stereo: balance). Per track: fader + balance, mute/solo. Then the
// master fader and a hard limit. Gains ramp across each block, so changes never click.
class SequenceAudioSource final : public AudioSource {
public:
    SequenceAudioSource(std::shared_ptr<const SequenceSnapshot> snapshot, int sampleRate, int channels);
    ~SequenceAudioSource() override;
    void seek(const Rational& t) override;
    std::size_t read(float* out, std::size_t frames) override;
    [[nodiscard]] std::vector<TrackLevel> trackLevels() const override { return m_levels; }
    // Swaps in a snapshot that passed sameExceptAudioMix(): new gains apply from the next block.
    void updateMix(std::shared_ptr<const SequenceSnapshot> snapshot);

private:
    struct ClipReader {
        std::unique_ptr<AudioSource> source; // a file, or a nested sequence (D30)
        std::int64_t nextFrame = -1;         // sequence frame the source is positioned at (100 %)
        // Retimed clips (D28): decoded source frames [windowStart, windowStart + window/ch) and the
        // source frame the source reads next.
        std::vector<float> window;
        std::int64_t windowStart = 0;
        std::int64_t readPos = -1;
    };

    // Fills m_scratch with `count` frames of a retimed clip starting at sequence frame `segStart`
    // (varispeed by linear interpolation: pitch follows the speed). Returns frames produced.
    std::size_t readRetimed(ClipReader& reader, const model::Clip& clip, std::int64_t segStart, std::size_t count);
    void ensureWindow(ClipReader& reader, std::int64_t lo, std::int64_t hi, bool backwards);

    void mixClip(const model::Clip& clip, const model::MediaItem& media, std::int64_t segStart, std::int64_t segEnd,
                 std::int64_t blockStart, float* bus, const std::vector<model::TransitionSpan>& spans);

    std::shared_ptr<const SequenceSnapshot> m_snapshot;
    int m_rate;
    int m_channels;
    std::int64_t m_position = 0;
    std::int64_t m_end = 0;
    std::unordered_map<model::ClipId, ClipReader> m_readers;
    std::unordered_set<model::MediaId> m_offline;
    std::vector<float> m_scratch;
    std::vector<float> m_bus;
    std::unordered_map<model::TrackId, audio::StereoGains> m_trackGains; // last applied (ramps)
    audio::StereoGains m_masterGain{0.0f, 0.0f};
    bool m_rampsValid = false; // false after a seek: start at the target gain
    std::vector<TrackLevel> m_levels;
};

} // namespace loki::playback
