#include "media/ffmpeg_utils.h"
#include "media/frame_cache.h"
#include "media/media_cache.h"
#include "media/thumbnail.h"
#include "media/waveform.h"
#include "util/atomic_file.h"

#include "media_assets.h"
#include "test_temp_dir.h"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>

namespace {

using namespace loki;
using namespace loki::media;
using loki::test::MediaAssets;

VideoFramePtr fakeFrame(std::size_t bytes) {
    auto f = std::make_shared<VideoFrame>();
    f->planeCount = 1;
    f->planes[0].resize(bytes);
    return f;
}

TEST(FrameCacheTest, LruEvictionByBytes) {
    FrameCache cache(300);
    cache.put({"a", 0, 1}, fakeFrame(100));
    cache.put({"a", 0, 2}, fakeFrame(100));
    cache.put({"a", 0, 3}, fakeFrame(100));
    EXPECT_EQ(cache.bytes(), 300u);
    EXPECT_NE(cache.get({"a", 0, 1}), nullptr); // 1 becomes most recent
    cache.put({"a", 0, 4}, fakeFrame(100));      // evicts 2 (least recent)
    EXPECT_EQ(cache.get({"a", 0, 2}), nullptr);
    EXPECT_NE(cache.get({"a", 0, 1}), nullptr);
    EXPECT_NE(cache.get({"a", 0, 4}), nullptr);
    EXPECT_EQ(cache.size(), 3u);
    EXPECT_GE(cache.hits(), 3u);
    EXPECT_GE(cache.misses(), 1u);
}

TEST(FrameCacheTest, KeysDistinguishSourceQualityAndFrame) {
    FrameCache cache(1000);
    cache.put({"a", 0, 1}, fakeFrame(10));
    EXPECT_EQ(cache.get({"b", 0, 1}), nullptr);
    EXPECT_EQ(cache.get({"a", 1, 1}), nullptr);
    EXPECT_EQ(cache.get({"a", 0, 2}), nullptr);
}

TEST(FrameCacheTest, ReplaceAndOversizedAndShrink) {
    FrameCache cache(100);
    cache.put({"a", 0, 1}, fakeFrame(50));
    cache.put({"a", 0, 1}, fakeFrame(70));
    EXPECT_EQ(cache.bytes(), 70u);
    cache.put({"a", 0, 2}, fakeFrame(500)); // larger than the cache: ignored
    EXPECT_EQ(cache.size(), 1u);
    cache.setMaxBytes(10);
    EXPECT_EQ(cache.size(), 0u);
    cache.put({"a", 0, 3}, nullptr);
    EXPECT_EQ(cache.size(), 0u);
}

TEST(FrameCacheTest, ConcurrentAccessIsSafe) {
    FrameCache cache(5000);
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&cache, t] {
            for (int i = 0; i < 2000; ++i) {
                cache.put({"s", t, i % 50}, fakeFrame(10));
                (void)cache.get({"s", (t + 1) % 4, i % 50});
            }
        });
    }
    for (auto& th : threads) {
        th.join();
    }
    EXPECT_LE(cache.bytes(), 5000u);
}

TEST(MediaCacheTest, KeyTracksFileIdentity) {
    test::TempDir dir;
    const auto file = dir.path() / "a.bin";
    util::writeFileAtomic(file, "one");
    const auto k1 = MediaCache::sourceKey(file);
    EXPECT_EQ(k1.size(), 16u);
    EXPECT_EQ(MediaCache::sourceKey(file), k1);
    util::writeFileAtomic(file, "longer content");
    EXPECT_NE(MediaCache::sourceKey(file), k1);
    EXPECT_TRUE(MediaCache::sourceKey(dir.path() / "missing").empty());
}

TEST(MediaCacheTest, EntryPathsUsageAndClear) {
    test::TempDir dir;
    const MediaCache cache(dir.path() / "cache");
    const auto entry = cache.entryPath("thumbnails", "abc", ".jpg");
    EXPECT_EQ(entry.filename(), "abc.jpg");
    EXPECT_TRUE(std::filesystem::is_directory(entry.parent_path()));
    util::writeFileAtomic(entry, std::string(1000, 'x'));
    EXPECT_EQ(cache.diskUsage(), 1000u);
    EXPECT_TRUE(cache.clear());
    EXPECT_EQ(cache.diskUsage(), 0u);
}

TEST(WaveformTest, ComputesPeaksOfSine) {
    const auto& a = MediaAssets::get();
    const auto wf = computeWaveform(a.wav, 480);
    ASSERT_TRUE(wf.has_value());
    EXPECT_EQ(wf->channels, 2);
    EXPECT_EQ(wf->frameCount, 96000);
    EXPECT_EQ(wf->peakCount(), 200u);
    EXPECT_EQ(wf->duration(), Rational(2));
    // 480 samples at 48 kHz = 10 ms = 4.4 periods of 440 Hz: each block reaches both peaks.
    for (std::size_t p = 0; p < wf->peakCount(); p += 13) {
        EXPECT_NEAR(wf->max(p, 0), 16383, 200);
        EXPECT_NEAR(wf->min(p, 1), -16383, 200);
    }
}

TEST(WaveformTest, MonoStaysMono) {
    const auto& a = MediaAssets::get();
    const auto wf = computeWaveform(a.monoWav, 256);
    ASSERT_TRUE(wf.has_value());
    EXPECT_EQ(wf->channels, 1);
}

TEST(WaveformTest, SaveLoadRoundTripAndCorruption) {
    const auto& a = MediaAssets::get();
    test::TempDir dir;
    const auto wf = computeWaveform(a.wav, 256);
    ASSERT_TRUE(wf.has_value());
    const auto file = dir.path() / "w.lkwf";
    saveWaveform(*wf, file);
    EXPECT_EQ(loadWaveform(file), *wf);

    auto bytes = util::readFile(file);
    util::writeFileAtomic(file, bytes.substr(0, bytes.size() - 3));
    EXPECT_THROW((void)loadWaveform(file), std::runtime_error);
    util::writeFileAtomic(file, "NOPE");
    EXPECT_THROW((void)loadWaveform(file), std::runtime_error);
}

TEST(WaveformTest, CancellationReturnsNullopt) {
    const auto& a = MediaAssets::get();
    util::TaskPool pool(1);
    std::atomic<bool> started{false};
    std::atomic<bool> go{false};
    std::optional<Waveform> result = Waveform{};
    auto handle = pool.submit([&](const util::CancelToken& token) {
        started = true;
        while (!go) {
            std::this_thread::yield();
        }
        result = computeWaveform(a.wav, 64, &token);
    });
    while (!started) {
        std::this_thread::yield();
    }
    handle.cancel();
    go = true;
    pool.waitIdle();
    EXPECT_FALSE(result.has_value());
}

TEST(ThumbnailTest, FitsBoundsAndIsRgba) {
    const auto& a = MediaAssets::get();
    const auto t = makeThumbnail(a.hd, Rational(1), 320, 180);
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->format, PixelFormat::Rgba);
    EXPECT_EQ(t->width, 320);
    EXPECT_EQ(t->height, 180);
    const auto still = makeThumbnail(a.png, Rational(), 320, 180);
    EXPECT_EQ(still->width, 64); // never upscaled
    const auto past = makeThumbnail(a.mpeg4WithAudio, Rational(500), 160, 120);
    EXPECT_NE(past, nullptr); // clamps into the stream
    EXPECT_THROW((void)makeThumbnail(a.wav, Rational(), 160, 120), MediaError);
}

} // namespace
