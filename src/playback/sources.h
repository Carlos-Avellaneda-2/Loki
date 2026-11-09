#pragma once

#include "media/video_frame.h"
#include "model/types.h"

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
    media::VideoFramePtr frame;
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
};

// Phase 5: shows the topmost enabled clip on a visible video track (no compositing yet).
class SequenceVideoSource final : public VideoSource {
public:
    SequenceVideoSource(std::shared_ptr<const SequenceSnapshot> snapshot, bool allowHardware, int maxWidth,
                        int maxHeight);
    ~SequenceVideoSource() override;
    std::optional<TimedFrame> frameAt(const Rational& t) override;
    void seek(const Rational& t) override;
    std::optional<TimedFrame> nextFrame() override;
    std::vector<TimedFrame> framesUpTo(const Rational& t) override;
    void setMaxSize(int maxWidth, int maxHeight) override;

private:
    media::VideoDecoder* decoderFor(const model::MediaItem& media);

    std::shared_ptr<const SequenceSnapshot> m_snapshot;
    bool m_allowHardware;
    int m_maxWidth;
    int m_maxHeight;
    Rational m_frameDuration;
    Rational m_next;
    std::list<std::pair<model::MediaId, std::unique_ptr<media::VideoDecoder>>> m_decoders; // LRU
    std::unordered_set<model::MediaId> m_offline;
};

// Phase 5: sums every enabled clip on unmuted audio tracks (unity gain, clamped). The Phase 8
// mixer adds gain, pan and meters.
class SequenceAudioSource final : public AudioSource {
public:
    SequenceAudioSource(std::shared_ptr<const SequenceSnapshot> snapshot, int sampleRate, int channels);
    ~SequenceAudioSource() override;
    void seek(const Rational& t) override;
    std::size_t read(float* out, std::size_t frames) override;

private:
    struct ClipReader {
        std::unique_ptr<media::AudioDecoder> decoder;
        std::int64_t nextFrame = -1; // sequence frame the decoder is positioned at
    };

    std::shared_ptr<const SequenceSnapshot> m_snapshot;
    int m_rate;
    int m_channels;
    std::int64_t m_position = 0;
    std::int64_t m_end = 0;
    std::unordered_map<model::ClipId, ClipReader> m_readers;
    std::unordered_set<model::MediaId> m_offline;
    std::vector<float> m_scratch;
};

} // namespace loki::playback
