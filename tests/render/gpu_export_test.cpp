// Export with the real OpenGL renderer and the best hardware encoder of this machine. Skipped
// without OpenGL 3.3 (CI). Part of loki_gl_tests (native platform, QGuiApplication in main).
#include "export/encoders.h"
#include "export/export_job.h"
#include "media/media_probe.h"
#include "model/project.h"

#include "media_assets.h"
#include "test_temp_dir.h"

#include <QOffscreenSurface>
#include <QSurfaceFormat>

#include <gtest/gtest.h>

#include <iostream>

namespace {

using namespace loki;
using namespace loki::exporter;

TEST(GpuExportTest, RendersWithOpenGlAndTheAutomaticEncoder) {
    QOffscreenSurface surface;
    QSurfaceFormat format;
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CompatibilityProfile);
    surface.setFormat(format);
    surface.create();
    auto renderer = makeGlRenderer(&surface);
    if (!renderer->start()) {
        GTEST_SKIP() << "OpenGL 3.3 not available";
    }
    renderer->stop();

    model::MediaItem video;
    video.id = model::MediaId::generate();
    video.path = loki::test::MediaAssets::get().mpeg4WithAudio;
    video.info = media::probeMedia(video.path);
    auto s = std::make_shared<playback::SequenceSnapshot>();
    s->sequence = model::makeDefaultSequence("S", {}, 1, 1);
    s->sequence.settings.width = 640;
    s->sequence.settings.height = 360;
    s->sequence.settings.frameRate = Rational(25);
    model::Clip c;
    c.id = model::ClipId::generate();
    c.media = video.id;
    c.duration = Rational(1);
    s->sequence.videoTracks[0].clips.push_back(c);
    s->media[video.id] = video;

    loki::test::TempDir dir;
    ExportRequest request;
    request.snapshot = s;
    request.out = Rational(1);
    request.file = dir.path() / "gpu.mp4";
    request.preset = *findExportPreset("h264-match");
    std::vector<std::unique_ptr<FrameRenderer>> renderers;
    renderers.push_back(makeGlRenderer(&surface));
    const auto result = ExportJob(request, std::move(renderers)).run();
    ASSERT_EQ(result.status, ExportStatus::Finished) << result.error;
    EXPECT_EQ(result.renderer, "GPU (OpenGL)");
    EXPECT_EQ(result.videoEncoder, pickEncoder(VideoCodec::H264));
    const auto info = media::probeMedia(request.file);
    EXPECT_EQ(info.width, 640);
    EXPECT_NEAR(info.duration.toDouble(), 1.0, 0.05);
    std::cout << "GPU export: " << result.frames << " frames in " << result.seconds << " s with "
              << result.videoEncoder << "\n";
}

} // namespace
