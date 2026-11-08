#pragma once

#include "util/rational.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace loki::test {

// Synthetic media written with the linked FFmpeg encoders (no binary assets in git, no ffmpeg CLI).
// Every video frame is a flat gray whose luma encodes its index (see expectedLuma), so decode
// tests can check that exactly the right frame came back.
struct VideoSpec {
    int width = 320;
    int height = 240;
    Rational frameRate{25, 1};
    int frames = 50;
    int gop = 12;
    int bFrames = 2;              // mpeg4 only
    std::string codec = "mpeg4";  // "mpeg4" or "libopenh264"
    bool withAudio = true;        // AAC stereo 48 kHz, 440 Hz sine
};

[[nodiscard]] constexpr int expectedLuma(std::int64_t frameIndex) noexcept {
    return 16 + static_cast<int>((frameIndex * 7) % 200);
}

// Writes an .mp4/.mkv/.mov (container from the extension). Throws on failure.
void generateVideo(const std::filesystem::path& file, const VideoSpec& spec);

// 16-bit PCM WAV with a sine wave at `frequency` Hz (amplitude 0.5).
void generateWav(const std::filesystem::path& file, double seconds, int sampleRate = 48000, int channels = 2,
                 double frequency = 440.0);

// Single-frame PNG (RGB gradient).
void generatePng(const std::filesystem::path& file, int width, int height);

} // namespace loki::test
