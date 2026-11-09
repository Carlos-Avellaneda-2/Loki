#pragma once

#include "util/rational.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace loki::exporter {

enum class VideoCodec { None, H264, Hevc, ProRes };
enum class AudioCodec { None, Aac, Pcm };

struct VideoSettings {
    VideoCodec codec = VideoCodec::H264;
    int width = 1920;
    int height = 1080;
    Rational frameRate{30};
    std::int64_t bitRate = 16'000'000; // H.264/HEVC target (bits per second)
    // Encoder name to use ("h264_qsv", "libopenh264", ...); empty = best available (see encoders.h).
    std::string encoder;
};

struct AudioSettings {
    AudioCodec codec = AudioCodec::Aac;
    int sampleRate = 48000;
    int channels = 2;
    std::int64_t bitRate = 192'000;
};

// Encodes RGBA pictures and interleaved float audio into a file (container from the extension:
// .mp4, .mov, .wav). LGPL-only encoders (D23): hardware H.264/HEVC, OpenH264, prores_ks, aac, pcm.
// Throws media::MediaError on failure.
class MediaWriter {
public:
    MediaWriter(const std::filesystem::path& file, const VideoSettings& video, const AudioSettings& audio);
    ~MediaWriter();
    MediaWriter(const MediaWriter&) = delete;
    MediaWriter& operator=(const MediaWriter&) = delete;
    MediaWriter(MediaWriter&&) = delete;
    MediaWriter& operator=(MediaWriter&&) = delete;

    // Next video frame: width*height*4 bytes, rows top to bottom.
    void writeVideo(const std::uint8_t* rgba, int stride);
    // Interleaved float samples (frames * channels).
    void writeAudio(const float* samples, std::size_t frames);
    // Flushes encoders and writes the trailer. Must be called once for a valid file.
    void finish();

    [[nodiscard]] std::string videoEncoderName() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace loki::exporter
