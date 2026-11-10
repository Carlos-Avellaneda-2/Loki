#include "export/encoders.h"
#include "export/export_job.h"
#include "media/media_probe.h"
#include "media/video_decoder.h"
#include "model/project.h"
#include "playback/render_previews.h"

#include "media_assets.h"
#include "media_generator.h"
#include "test_temp_dir.h"

#include <gtest/gtest.h>

namespace {

using namespace loki;
using namespace loki::exporter;
using loki::test::expectedLuma;
using loki::test::MediaAssets;

model::MediaItem mediaItem(const std::filesystem::path& path) {
    model::MediaItem m;
    m.id = model::MediaId::generate();
    m.name = path.stem().string();
    m.path = path;
    m.info = media::probeMedia(path);
    return m;
}

class ExportTest : public ::testing::Test {
protected:
    loki::test::TempDir dir;
    model::MediaItem video = mediaItem(MediaAssets::get().mpeg4WithAudio); // 320x240, 25 fps, 2 s, AAC

    ExportRequest request(const std::string& presetId, const std::string& file) {
        auto s = std::make_shared<playback::SequenceSnapshot>();
        s->sequence = model::makeDefaultSequence("S", {}, 1, 1);
        s->sequence.settings.width = 320;
        s->sequence.settings.height = 240;
        s->sequence.settings.frameRate = Rational(25);
        model::Clip v;
        v.id = model::ClipId::generate();
        v.media = video.id;
        v.start = Rational(0);
        v.duration = Rational(1);
        v.sourceIn = Rational(1, 5); // starts at source frame 5
        auto a = v;
        a.id = model::ClipId::generate();
        s->sequence.videoTracks[0].clips.push_back(v);
        s->sequence.audioTracks[0].clips.push_back(a);
        s->media[video.id] = video;
        ExportRequest r;
        r.snapshot = s;
        r.in = Rational(0);
        r.out = Rational(1);
        r.file = dir.path() / file;
        r.preset = *findExportPreset(presetId);
        return r;
    }
    static std::vector<std::unique_ptr<FrameRenderer>> cpu() {
        std::vector<std::unique_ptr<FrameRenderer>> r;
        r.push_back(makeCpuRenderer());
        return r;
    }
    static int centerLuma(const media::VideoFramePtr& f) {
        return f ? f->planes[0][static_cast<std::size_t>(f->strides[0]) * static_cast<std::size_t>(f->height / 2) +
                                static_cast<std::size_t>(f->width / 2)]
                 : -1;
    }
};

TEST_F(ExportTest, EncoderCandidatesAreLgplOnly) {
    for (const auto codec : {VideoCodec::H264, VideoCodec::Hevc, VideoCodec::ProRes}) {
        for (const auto& name : encoderCandidates(codec)) {
            EXPECT_EQ(name.find("x264"), std::string::npos);
            EXPECT_EQ(name.find("x265"), std::string::npos);
        }
    }
    EXPECT_FALSE(pickEncoder(VideoCodec::H264).empty()); // OpenH264 at least
    EXPECT_EQ(pickEncoder(VideoCodec::ProRes), "prores_ks");
}

TEST_F(ExportTest, PresetFrameRateResamplesTheTimeline) {
    auto r = request("h264-1080p60", "60p.mp4");
    EXPECT_EQ(ExportJob::frameRate(r), Rational(60));
    EXPECT_EQ(ExportJob::frameCount(r), 60);
    EXPECT_EQ(ExportJob::outputSize(r), std::make_pair(1920, 1080));
    const auto* k4 = findExportPreset("h264-4k30");
    ASSERT_NE(k4, nullptr);
    EXPECT_EQ(k4->width, 3840);
    EXPECT_EQ(k4->frameRate, Rational(30));
    r.encoder = "libopenh264";
    r.out = Rational(1, 2);
    r.preset.width = 320; // keep the test fast: same timing, small picture
    r.preset.height = 240;
    const auto result = ExportJob(r, cpu()).run();
    ASSERT_EQ(result.status, ExportStatus::Finished) << result.error;
    EXPECT_EQ(result.frames, 30);
    EXPECT_EQ(media::probeMedia(r.file).frameRate, Rational(60));
}

TEST_F(ExportTest, H264ExportMatchesTheSequence) {
    auto r = request("h264-match", "out.mp4");
    r.encoder = "libopenh264"; // deterministic on every machine
    ExportJob job(r, cpu());
    const auto result = job.run();
    ASSERT_EQ(result.status, ExportStatus::Finished) << result.error;
    EXPECT_EQ(result.frames, 25);
    EXPECT_EQ(result.renderer, "CPU");
    EXPECT_DOUBLE_EQ(job.progress(), 1.0);
    const auto info = media::probeMedia(r.file);
    EXPECT_TRUE(info.hasVideo);
    EXPECT_TRUE(info.hasAudio);
    EXPECT_EQ(info.width, 320);
    EXPECT_EQ(info.height, 240);
    EXPECT_EQ(info.frameRate, Rational(25));
    EXPECT_NEAR(info.duration.toDouble(), 1.0, 0.05);
    // Pictures come from the right source frames (clip starts at source frame 5).
    media::VideoDecoder decoder(r.file, {});
    for (const int i : {0, 12, 24}) {
        EXPECT_NEAR(centerLuma(decoder.frameAt(Rational(i, 25))), expectedLuma(5 + i), 6) << "frame " << i;
    }
}

TEST_F(ExportTest, ProResAndAudioOnlyPresets) {
    auto prores = request("prores-hq", "out.mov");
    const auto p = ExportJob(prores, cpu()).run();
    ASSERT_EQ(p.status, ExportStatus::Finished) << p.error;
    EXPECT_EQ(p.videoEncoder, "prores_ks");
    const auto pInfo = media::probeMedia(prores.file);
    EXPECT_EQ(pInfo.videoCodec, "prores");
    EXPECT_TRUE(pInfo.hasAudio);
    auto wav = request("wav", "out.wav");
    wav.out = Rational(1, 2);
    const auto w = ExportJob(wav, cpu()).run();
    ASSERT_EQ(w.status, ExportStatus::Finished) << w.error;
    const auto wInfo = media::probeMedia(wav.file);
    EXPECT_FALSE(wInfo.hasVideo);
    EXPECT_EQ(wInfo.sampleRate, 48000);
    EXPECT_NEAR(wInfo.duration.toDouble(), 0.5, 0.01);
}

TEST_F(ExportTest, CancelAndFailureRemoveThePartialFile) {
    auto r = request("h264-match", "cancelled.mp4");
    r.encoder = "libopenh264";
    ExportJob job(r, cpu());
    job.cancel();
    EXPECT_EQ(job.run().status, ExportStatus::Cancelled);
    EXPECT_FALSE(std::filesystem::exists(r.file));
    auto bad = request("h264-match", "bad.mp4");
    bad.encoder = "no_such_encoder";
    const auto failed = ExportJob(bad, cpu()).run();
    EXPECT_EQ(failed.status, ExportStatus::Failed);
    EXPECT_FALSE(failed.error.empty());
    EXPECT_FALSE(std::filesystem::exists(bad.file));
    auto empty = request("h264-match", "empty.mp4");
    empty.out = Rational(0);
    EXPECT_EQ(ExportJob(empty, cpu()).run().status, ExportStatus::Failed);
}

TEST_F(ExportTest, RenderPreviewIsUsedByPlaybackUntilTheSegmentChanges) {
    auto r = request("h264-match", "unused.mp4");
    auto snapshot = std::const_pointer_cast<playback::SequenceSnapshot>(r.snapshot);
    const auto key = playback::previewSegmentKey(*snapshot, 0);
    ASSERT_FALSE(key.empty());
    const auto cache = dir.path() / "render";
    std::filesystem::create_directories(cache);
    r.preset = renderPreviewPreset();
    r.file = playback::previewFile(cache, key);
    ExportJob job(r, cpu());
    ASSERT_EQ(job.run().status, ExportStatus::Finished);
    playback::attachPreviews(*snapshot, cache);
    ASSERT_EQ(snapshot->previews.files.size(), 1u);
    EXPECT_FALSE(snapshot->previews.files[0].empty());
    playback::SequenceVideoSource src(snapshot, false, 0, 0);
    const auto f = src.frameAt(Rational(10, 25));
    ASSERT_TRUE(f.has_value() && f->composite);
    ASSERT_EQ(f->composite->layers.size(), 1u);
    EXPECT_NEAR(centerLuma(f->frame), loki::test::expectedLuma(15), 8); // source frame 5 + 10
    EXPECT_EQ(playback::previewStates(*snapshot, cache)[0], playback::PreviewState::Rendered);
    // An edit in the segment changes its key: the old render is no longer used.
    snapshot->sequence.videoTracks[0].clips[0].enabled = false;
    playback::attachPreviews(*snapshot, cache);
    EXPECT_TRUE(snapshot->previews.files.empty() || snapshot->previews.files[0].empty());
}

} // namespace
