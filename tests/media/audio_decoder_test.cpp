#include "media/audio_decoder.h"

#include "media_assets.h"

#include <gtest/gtest.h>

#include <cmath>
#include <numbers>
#include <vector>

namespace {

using namespace loki;
using namespace loki::media;
using loki::test::MediaAssets;

double sineAt(std::int64_t sample, int rate, double freq) {
    return 0.5 * std::sin(2.0 * std::numbers::pi * freq * static_cast<double>(sample) / rate);
}

TEST(AudioDecoderTest, ReadsWholeWavExactly) {
    const auto& a = MediaAssets::get();
    AudioDecoder d(a.wav, 48000, 2);
    EXPECT_EQ(d.info().sourceSampleRate, 48000);
    EXPECT_EQ(d.info().sourceChannels, 2);
    std::vector<float> buffer(4096 * 2);
    std::int64_t total = 0;
    while (const auto n = d.read(buffer.data(), 4096)) {
        for (std::size_t i = 0; i < n; i += 97) {
            ASSERT_NEAR(buffer[i * 2], sineAt(total + static_cast<std::int64_t>(i), 48000, 440.0), 1e-3);
            ASSERT_FLOAT_EQ(buffer[i * 2], buffer[i * 2 + 1]);
        }
        total += static_cast<std::int64_t>(n);
    }
    EXPECT_EQ(total, 96000);
    EXPECT_EQ(d.position(), Rational(2));
}

TEST(AudioDecoderTest, SeekIsSampleAccurate) {
    const auto& a = MediaAssets::get();
    AudioDecoder d(a.wav, 48000, 2);
    for (const std::int64_t target : {0LL, 1LL, 12345LL, 48000LL, 95000LL}) {
        d.seek(Rational(target, 48000));
        EXPECT_EQ(d.position(), Rational(target, 48000));
        std::vector<float> buffer(64 * 2);
        const auto n = d.read(buffer.data(), 64);
        ASSERT_GT(n, 0u);
        for (std::size_t i = 0; i < n; ++i) {
            ASSERT_NEAR(buffer[i * 2], sineAt(target + static_cast<std::int64_t>(i), 48000, 440.0), 1e-3)
                << "target " << target << " sample " << i;
        }
    }
}

TEST(AudioDecoderTest, ResamplesAndUpmixesMono) {
    const auto& a = MediaAssets::get();
    AudioDecoder d(a.monoWav, 48000, 2);
    std::vector<float> buffer(48000 * 2);
    std::int64_t total = 0;
    while (const auto n = d.read(buffer.data(), 48000)) {
        total += static_cast<std::int64_t>(n);
    }
    EXPECT_NEAR(static_cast<double>(total), 48000.0, 64.0); // resampler edge samples
}

TEST(AudioDecoderTest, KeepsSourceChannelsWhenAsked) {
    const auto& a = MediaAssets::get();
    AudioDecoder d(a.monoWav, 44100, 0);
    EXPECT_EQ(d.channels(), 1);
}

TEST(AudioDecoderTest, DecodesAacFromVideoFile) {
    const auto& a = MediaAssets::get();
    AudioDecoder d(a.mpeg4WithAudio, 48000, 2);
    EXPECT_EQ(d.info().codec, "aac");
    d.seek(Rational(1));
    std::vector<float> buffer(1024 * 2);
    ASSERT_EQ(d.read(buffer.data(), 1024), 1024u);
    float peak = 0;
    for (const float v : buffer) {
        peak = std::max(peak, std::abs(v));
    }
    EXPECT_NEAR(peak, 0.5f, 0.05f);
}

TEST(AudioDecoderTest, ThrowsWithoutAudio) {
    const auto& a = MediaAssets::get();
    EXPECT_THROW(AudioDecoder(a.h264), MediaError);
}

} // namespace
