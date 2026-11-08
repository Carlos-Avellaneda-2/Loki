#include "media_assets.h"

#include <chrono>
#include <fstream>
#include <system_error>

namespace loki::test {

const MediaAssets& MediaAssets::get() {
    static const MediaAssets assets; // thread-safe lazy init, destroyed at exit
    return assets;
}

MediaAssets::MediaAssets() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    dir = std::filesystem::temp_directory_path() / "loki_tests" / ("media_" + std::to_string(stamp));
    std::filesystem::create_directories(dir);

    mpeg4Spec = VideoSpec{};
    mpeg4WithAudio = dir / "mpeg4_aac.mp4";
    generateVideo(mpeg4WithAudio, mpeg4Spec);

    h264Spec.codec = "libopenh264";
    h264Spec.frameRate = Rational(30000, 1001);
    h264Spec.frames = 60;
    h264Spec.gop = 15;
    h264Spec.withAudio = false;
    h264 = dir / "openh264.mp4";
    generateVideo(h264, h264Spec);

    hdSpec.width = 1920;
    hdSpec.height = 1080;
    hdSpec.frameRate = Rational(30);
    hdSpec.frames = 90;
    hdSpec.gop = 30;
    hdSpec.bFrames = 0;
    hdSpec.withAudio = false;
    hd = dir / "hd.mp4";
    generateVideo(hd, hdSpec);

    wav = dir / "tone.wav";
    generateWav(wav, 2.0, 48000, 2, 440.0);
    monoWav = dir / "mono.wav";
    generateWav(monoWav, 1.0, 44100, 1, 1000.0);
    png = dir / "still.png";
    generatePng(png, 64, 48);
    notMedia = dir / "notes.txt";
    std::ofstream(notMedia) << "this is not media";
}

MediaAssets::~MediaAssets() {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

} // namespace loki::test
