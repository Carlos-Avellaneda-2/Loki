#pragma once

#include "media/ffmpeg_utils.h"

#include <cstddef>
#include <filesystem>
#include <vector>

namespace loki::media {

struct AudioStreamInfo {
    int sourceSampleRate = 0;
    int sourceChannels = 0;
    Rational duration;
    std::string codec;
};

// Decodes the best audio stream of a file to interleaved float32 at a fixed output rate and
// channel count (resampled with swresample). Sample-accurate seek. Not thread-safe.
class AudioDecoder {
public:
    // channels <= 0 keeps the source channel count.
    AudioDecoder(const std::filesystem::path& path, int sampleRate = 48000, int channels = 2);
    ~AudioDecoder();
    AudioDecoder(const AudioDecoder&) = delete;
    AudioDecoder& operator=(const AudioDecoder&) = delete;
    AudioDecoder(AudioDecoder&&) = delete;
    AudioDecoder& operator=(AudioDecoder&&) = delete;

    [[nodiscard]] const AudioStreamInfo& info() const noexcept { return m_info; }
    [[nodiscard]] int sampleRate() const noexcept { return m_outRate; }
    [[nodiscard]] int channels() const noexcept { return m_outChannels; }

    // Next read() starts exactly at media time `t` (silence is produced before the first sample).
    void seek(const Rational& t);
    // Reads up to `frames` frames (frames * channels floats). Returns frames read; 0 at end.
    std::size_t read(float* out, std::size_t frames);
    // Media time of the next frame read() will return.
    [[nodiscard]] Rational position() const;

private:
    bool decodeMore(); // appends converted samples to m_buffer; false at end of stream
    void resetResampler();

    FormatContextPtr m_format;
    CodecContextPtr m_codec;
    SwrContextPtr m_swr;
    PacketPtr m_packet;
    FramePtr m_frame;
    int m_streamIndex = -1;
    AVRational m_timeBase{0, 1};
    Rational m_startTime;
    AudioStreamInfo m_info;
    int m_outRate;
    int m_outChannels;
    std::vector<float> m_buffer; // decoded, not yet read (interleaved)
    std::size_t m_bufferOffset = 0; // in floats
    std::int64_t m_position = 0;     // output frames: index of the next frame read() returns
    std::int64_t m_decodedEnd = -1;  // output frame index just after the buffered samples (-1 = unknown)
    std::int64_t m_seekTarget = 0;   // output frame index requested by the last seek
    bool m_eof = false;
    bool m_draining = false;
};

} // namespace loki::media
