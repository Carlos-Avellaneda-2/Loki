#pragma once

#include "media_generator.h"

#include <filesystem>
#include <string>

namespace loki::test {

// Lazily generated, process-wide test media (created once in the temp folder, deleted at exit).
class MediaAssets {
public:
    static const MediaAssets& get();

    std::filesystem::path dir;
    std::filesystem::path mpeg4WithAudio;  // 320x240, 25 fps, 50 frames, GOP 12, B-frames, AAC
    std::filesystem::path h264;            // 320x240, 30000/1001 fps, 60 frames, GOP 15, openh264, no audio
    std::filesystem::path hd;              // 1920x1080, 30 fps, 90 frames, GOP 30 (scrub latency)
    std::filesystem::path wav;             // 2 s stereo 48 kHz sine 440 Hz
    std::filesystem::path monoWav;         // 1 s mono 44.1 kHz
    std::filesystem::path png;             // 64x48 still
    std::filesystem::path notMedia;        // text file

    VideoSpec mpeg4Spec;
    VideoSpec h264Spec;
    VideoSpec hdSpec;

    MediaAssets(const MediaAssets&) = delete;
    MediaAssets& operator=(const MediaAssets&) = delete;
    MediaAssets(MediaAssets&&) = delete;
    MediaAssets& operator=(MediaAssets&&) = delete;
    ~MediaAssets();

private:
    MediaAssets();
};

} // namespace loki::test
