#include "media/media_probe.h"
#include "model/project.h"
#include "playback/performance.h"
#include "playback/sources.h"

#include "media_assets.h"
#include "media_generator.h"

#include <gtest/gtest.h>

namespace {

using namespace loki;
using namespace loki::playback;
using loki::test::expectedLuma;
using loki::test::MediaAssets;

constexpr std::uint64_t kGB = 1024ull * 1024ull * 1024ull;

TEST(PerformanceProfileTest, AutoPicksByCoresAndMemory) {
    EXPECT_EQ(performanceProfile(PerformanceMode::Auto, {4, 16 * kGB}).mode, PerformanceMode::Standard);
    EXPECT_EQ(performanceProfile(PerformanceMode::Auto, {16, 8 * kGB}).mode, PerformanceMode::Standard);
    const auto high = performanceProfile(PerformanceMode::Auto, {16, 32 * kGB});
    EXPECT_EQ(high.mode, PerformanceMode::High);
    EXPECT_EQ(high.frameCacheBytes, 4 * kGB);
    EXPECT_TRUE(high.parallelPlaybackDecode);
    EXPECT_GT(high.maxVideoDecoders, 8u);
    // Forced High on a small machine stays within a sane cache; unknown RAM uses 1 GB.
    EXPECT_EQ(performanceProfile(PerformanceMode::High, {4, 8 * kGB}).frameCacheBytes, 1 * kGB);
    EXPECT_EQ(performanceProfile(PerformanceMode::High, {4, 0}).frameCacheBytes, 1 * kGB);
    const auto standard = performanceProfile(PerformanceMode::Standard, {64, 128 * kGB});
    EXPECT_FALSE(standard.parallelPlaybackDecode);
    EXPECT_EQ(standard.maxVideoDecoders, 8u);
    const auto hw = detectHardware();
    EXPECT_GE(hw.threads, 1u);
}

// Many layers with few decoders (re-opening) or decoded in parallel must give the same pictures.
TEST(PerformanceProfileTest, LayersAreRightWithAnyDecoderBudgetAndParallelPlayback) {
    model::MediaItem video;
    video.id = model::MediaId::generate();
    video.path = MediaAssets::get().mpeg4WithAudio; // 25 fps, 2 s
    video.info = media::probeMedia(video.path);
    auto s = std::make_shared<SequenceSnapshot>();
    s->sequence = model::makeDefaultSequence("S", {}, 4, 0);
    s->sequence.settings.frameRate = Rational(25);
    s->media[video.id] = video;
    for (int v = 0; v < 4; ++v) {
        model::Clip c;
        c.id = model::ClipId::generate();
        c.media = video.id;
        c.duration = Rational(1);
        c.sourceIn = Rational(v * 5, 25); // each layer 5 frames further into the source
        s->sequence.videoTracks[static_cast<std::size_t>(v)].clips.push_back(c);
    }
    const auto luma = [](const render::Layer& l) {
        return l.frame->planes[0][l.frame->planes[0].size() / 2];
    };
    for (const auto& [decoders, parallel] : {std::pair<std::size_t, bool>{2, false}, {32, true}}) {
        SequenceVideoSource src(s, false, 0, 0, decoders, parallel);
        src.seek(Rational(0));
        for (int i = 0; i < 6; ++i) {
            const auto f = src.nextFrame();
            ASSERT_TRUE(f && f->composite);
            ASSERT_EQ(f->composite->layers.size(), 4u);
            for (int v = 0; v < 4; ++v) {
                EXPECT_NEAR(luma(f->composite->layers[static_cast<std::size_t>(v)]), expectedLuma(i + v * 5), 3)
                    << "decoders " << decoders << " frame " << i << " layer " << v;
            }
        }
    }
}

} // namespace
