#include "media/ffmpeg_utils.h"
#include "media/media_probe.h"

#include "media_assets.h"

#include <gtest/gtest.h>

namespace {

using namespace loki;
using loki::test::MediaAssets;

TEST(MediaProbeTest, VideoWithAudio) {
    const auto& a = MediaAssets::get();
    const auto info = media::probeMedia(a.mpeg4WithAudio);
    EXPECT_TRUE(info.hasVideo);
    EXPECT_TRUE(info.hasAudio);
    EXPECT_FALSE(info.isStill);
    EXPECT_EQ(info.width, 320);
    EXPECT_EQ(info.height, 240);
    EXPECT_EQ(info.frameRate, Rational(25));
    EXPECT_EQ(info.videoCodec, "mpeg4");
    EXPECT_EQ(info.audioCodec, "aac");
    EXPECT_EQ(info.sampleRate, 48000);
    EXPECT_EQ(info.audioChannels, 2);
    // 50 frames at 25 fps; containers may round the duration slightly.
    EXPECT_NEAR(info.duration.toDouble(), 2.0, 0.1);
}

TEST(MediaProbeTest, NtscH264) {
    const auto& a = MediaAssets::get();
    const auto info = media::probeMedia(a.h264);
    EXPECT_TRUE(info.hasVideo);
    EXPECT_FALSE(info.hasAudio);
    EXPECT_EQ(info.videoCodec, "h264");
    EXPECT_EQ(info.frameRate, Rational(30000, 1001));
}

TEST(MediaProbeTest, AudioOnly) {
    const auto& a = MediaAssets::get();
    const auto info = media::probeMedia(a.wav);
    EXPECT_FALSE(info.hasVideo);
    EXPECT_TRUE(info.hasAudio);
    EXPECT_EQ(info.audioCodec, "pcm_s16le");
    EXPECT_EQ(info.duration, Rational(2));
    const auto mono = media::probeMedia(a.monoWav);
    EXPECT_EQ(mono.audioChannels, 1);
    EXPECT_EQ(mono.sampleRate, 44100);
}

TEST(MediaProbeTest, StillImage) {
    const auto& a = MediaAssets::get();
    const auto info = media::probeMedia(a.png);
    EXPECT_TRUE(info.hasVideo);
    EXPECT_TRUE(info.isStill);
    EXPECT_EQ(info.width, 64);
    EXPECT_EQ(info.height, 48);
    EXPECT_TRUE(info.duration.isZero());
}

TEST(MediaProbeTest, RejectsNonMediaAndMissingFiles) {
    const auto& a = MediaAssets::get();
    EXPECT_THROW((void)media::probeMedia(a.notMedia), media::MediaError);
    EXPECT_THROW((void)media::probeMedia(a.dir / "missing.mp4"), media::MediaError);
}

TEST(MediaProbeTest, ImportableExtensionsAreLowercaseWithDot) {
    for (const auto& ext : media::importableExtensions()) {
        ASSERT_FALSE(ext.empty());
        EXPECT_EQ(ext.front(), '.');
        for (const char c : ext) {
            EXPECT_FALSE(c >= 'A' && c <= 'Z');
        }
    }
}

TEST(FfmpegUtilsTest, RationalConversions) {
    EXPECT_EQ(media::toRational(AVRational{30000, 1001}), Rational(30000, 1001));
    EXPECT_EQ(media::toRational(AVRational{1, 0}), Rational());
    const auto av = media::toAVRational(Rational(24000, 1001));
    EXPECT_EQ(av.num, 24000);
    EXPECT_EQ(av.den, 1001);
    EXPECT_THROW((void)media::toAVRational(Rational(1, 1LL << 40)), media::MediaError);
    EXPECT_EQ(media::timestampToTime(90000, AVRational{1, 90000}), Rational(1));
    EXPECT_EQ(media::timeToTimestamp(Rational(1001, 30000), AVRational{1, 90000}), 3003);
}

} // namespace
