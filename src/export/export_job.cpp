#include "export/export_job.h"

#include "media/ffmpeg_utils.h"
#include "render/gl_compositor.h"

#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QSurfaceFormat>

#include <spdlog/spdlog.h>

#include <chrono>
#include <cstring>

namespace loki::exporter {

// --- Presets ------------------------------------------------------------------------------------

const std::vector<ExportPreset>& exportPresets() {
    static const std::vector<ExportPreset> kPresets = {
        {"h264-match", "H.264 - Match Sequence", ".mp4", VideoCodec::H264, AudioCodec::Aac, 0, 0, 0},
        {"h264-1080p30", "H.264 - 1080p30", ".mp4", VideoCodec::H264, AudioCodec::Aac, 1920, 1080, 16'000'000, Rational(30)},
        {"h264-1080p60", "H.264 - 1080p60", ".mp4", VideoCodec::H264, AudioCodec::Aac, 1920, 1080, 24'000'000, Rational(60)},
        {"h264-4k30", "H.264 - 4K UHD 30", ".mp4", VideoCodec::H264, AudioCodec::Aac, 3840, 2160, 45'000'000, Rational(30)},
        {"h264-1440p60", "H.264 - 1440p60", ".mp4", VideoCodec::H264, AudioCodec::Aac, 2560, 1440, 32'000'000, Rational(60)},
        {"h264-4k60", "H.264 - 4K UHD 60", ".mp4", VideoCodec::H264, AudioCodec::Aac, 3840, 2160, 68'000'000, Rational(60)},
        {"h264-720p", "H.264 - 720p", ".mp4", VideoCodec::H264, AudioCodec::Aac, 1280, 720, 8'000'000, {}},
        {"hevc-4k30", "HEVC (hardware) - 4K UHD 30", ".mp4", VideoCodec::Hevc, AudioCodec::Aac, 3840, 2160, 30'000'000, Rational(30)},
        {"hevc-4k60", "HEVC (hardware) - 4K UHD 60", ".mp4", VideoCodec::Hevc, AudioCodec::Aac, 3840, 2160, 45'000'000, Rational(60)},
        {"hevc-match", "HEVC (hardware) - Match Sequence", ".mp4", VideoCodec::Hevc, AudioCodec::Aac, 0, 0, 0},
        {"prores-hq", "ProRes 422 HQ (.mov)", ".mov", VideoCodec::ProRes, AudioCodec::Pcm, 0, 0, 0},
        {"wav", "WAV - Audio Only", ".wav", VideoCodec::None, AudioCodec::Pcm, 0, 0, 0},
    };
    return kPresets;
}

const ExportPreset* findExportPreset(const std::string& id) {
    for (const auto& p : exportPresets()) {
        if (p.id == id) {
            return &p;
        }
    }
    return nullptr;
}

// --- Renderers ------------------------------------------------------------------------------------

namespace {

class CpuRenderer final : public FrameRenderer {
public:
    bool start() override { return true; }
    render::CpuImage render(const render::CompositeFrame& frame, int width, int height) override {
        return render::compositeCpu(frame, width, height);
    }
    void stop() override {}
    [[nodiscard]] std::string name() const override { return "CPU"; }
};

class GlRenderer final : public FrameRenderer {
public:
    explicit GlRenderer(QOffscreenSurface* surface) : m_surface(surface) {}
    ~GlRenderer() override { stop(); }

    bool start() override {
        if (m_surface == nullptr || !m_surface->isValid()) {
            return false;
        }
        m_context = std::make_unique<QOpenGLContext>();
        QSurfaceFormat format;
        format.setVersion(3, 3);
        format.setProfile(QSurfaceFormat::CompatibilityProfile);
        m_context->setFormat(format);
        if (!m_context->create() || !m_context->makeCurrent(m_surface)) {
            m_context.reset();
            return false;
        }
        const auto f = m_context->format();
        if (f.majorVersion() * 10 + f.minorVersion() < 33 || !m_compositor.initialize()) {
            m_context->doneCurrent();
            m_context.reset();
            return false;
        }
        return true;
    }

    render::CpuImage render(const render::CompositeFrame& frame, int width, int height) override {
        m_compositor.render(frame, width, height);
        const QImage image = m_compositor.readback();
        render::CpuImage out;
        out.width = image.width();
        out.height = image.height();
        out.rgba.resize(static_cast<std::size_t>(out.width) * static_cast<std::size_t>(out.height) * 4);
        for (int y = 0; y < out.height; ++y) {
            std::memcpy(out.rgba.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(out.width) * 4,
                        image.constScanLine(y), static_cast<std::size_t>(out.width) * 4);
        }
        return out;
    }

    void stop() override {
        if (m_context) {
            m_compositor.release();
            m_context->doneCurrent();
            m_context.reset();
        }
    }

    [[nodiscard]] std::string name() const override { return "GPU (OpenGL)"; }

private:
    QOffscreenSurface* m_surface;
    std::unique_ptr<QOpenGLContext> m_context;
    render::GlCompositor m_compositor;
};

} // namespace

std::unique_ptr<FrameRenderer> makeCpuRenderer() {
    return std::make_unique<CpuRenderer>();
}

std::unique_ptr<FrameRenderer> makeGlRenderer(QOffscreenSurface* surface) {
    return std::make_unique<GlRenderer>(surface);
}

// --- Job ------------------------------------------------------------------------------------------

ExportJob::ExportJob(ExportRequest request, std::vector<std::unique_ptr<FrameRenderer>> renderers)
    : m_request(std::move(request)), m_renderers(std::move(renderers)) {}

std::pair<int, int> ExportJob::outputSize(const ExportRequest& request) {
    const auto& settings = request.snapshot->sequence.settings;
    int w = request.preset.width > 0 ? request.preset.width : settings.width;
    int h = request.preset.height > 0 ? request.preset.height : settings.height;
    return {w - w % 2, h - h % 2}; // encoders need even sizes
}

Rational ExportJob::frameRate(const ExportRequest& request) {
    return request.preset.frameRate.isPositive() ? request.preset.frameRate
                                                 : request.snapshot->sequence.settings.frameRate;
}

std::int64_t ExportJob::frameCount(const ExportRequest& request) {
    const auto rate = frameRate(request);
    return std::max<std::int64_t>(0, timeToFrames(request.out - request.in, rate, Rounding::Ceil));
}

ExportResult ExportJob::run() {
    ExportResult result;
    const auto started = std::chrono::steady_clock::now();
    const auto& seq = m_request.snapshot->sequence;
    const auto rate = frameRate(m_request); // a preset may resample the timeline (1080p60 from a 30p sequence)
    const auto [width, height] = outputSize(m_request);
    const auto frames = frameCount(m_request);
    const bool hasVideo = m_request.preset.video != VideoCodec::None;
    const bool hasAudio = m_request.preset.audio != AudioCodec::None;
    constexpr int kSampleRate = 48000;
    constexpr int kChannels = 2;

    FrameRenderer* renderer = nullptr;
    try {
        if (frames <= 0) {
            throw media::MediaError("The export range is empty");
        }
        if (hasVideo) {
            for (auto& r : m_renderers) {
                if (r->start()) {
                    renderer = r.get();
                    break;
                }
            }
            if (renderer == nullptr) {
                throw media::MediaError("No renderer available");
            }
            result.renderer = renderer->name();
        }
        VideoSettings video;
        video.codec = m_request.preset.video;
        video.width = width;
        video.height = height;
        video.frameRate = rate;
        video.encoder = m_request.encoder;
        // Default bit rate: about 0.12 bits per pixel per frame (16 Mb/s at 1080p30).
        video.bitRate = m_request.preset.videoBitRate > 0
                            ? m_request.preset.videoBitRate
                            : static_cast<std::int64_t>(0.12 * width * height * rate.toDouble());
        AudioSettings audio;
        audio.codec = m_request.preset.audio;
        audio.sampleRate = kSampleRate;
        audio.channels = kChannels;
        std::optional<MediaWriter> writer;
        writer.emplace(m_request.file, video, audio);
        result.videoEncoder = writer->videoEncoderName();

        // Full-quality decoding (no preview downscale); hardware decoding is allowed.
        playback::SequenceVideoSource videoSource(m_request.snapshot, true, 0, 0);
        playback::SequenceAudioSource audioSource(m_request.snapshot, kSampleRate, kChannels);
        audioSource.seek(m_request.in);
        const auto totalAudio = timeToFrames(m_request.out - m_request.in, Rational(kSampleRate), Rounding::Nearest);
        std::int64_t audioWritten = 0;
        std::vector<float> audioBuffer;
        render::CompositeFrame black{seq.settings.width, seq.settings.height, {}};

        for (std::int64_t i = 0; i < frames; ++i) {
            if (m_cancel.load()) {
                result.status = ExportStatus::Cancelled;
                break;
            }
            const Rational t = m_request.in + framesToTime(i, rate);
            if (hasVideo) {
                const auto picture = videoSource.frameAt(t);
                const auto& composite = picture && picture->composite ? *picture->composite : black;
                const auto image = renderer->render(composite, width, height);
                writer->writeVideo(image.rgba.data(), image.width * 4);
            }
            if (hasAudio) {
                // Audio up to the end of this frame (exact sample positions, no drift).
                const auto target = std::min(totalAudio, timeToFrames(framesToTime(i + 1, rate), Rational(kSampleRate),
                                                                      Rounding::Nearest));
                const auto need = static_cast<std::size_t>(std::max<std::int64_t>(0, target - audioWritten));
                if (need > 0) {
                    audioBuffer.assign(need * kChannels, 0.0f);
                    (void)audioSource.read(audioBuffer.data(), need); // silence past the sequence end
                    writer->writeAudio(audioBuffer.data(), need);
                    audioWritten += static_cast<std::int64_t>(need);
                }
            }
            result.frames = i + 1;
            m_progress.store(static_cast<double>(i + 1) / static_cast<double>(frames));
        }
        if (result.status != ExportStatus::Cancelled) {
            writer->finish();
            result.status = ExportStatus::Finished;
        }
        writer.reset();
    } catch (const std::exception& e) {
        result.status = ExportStatus::Failed;
        result.error = e.what();
        spdlog::error("Export failed: {}", e.what());
    }
    if (renderer != nullptr) {
        renderer->stop();
    }
    if (result.status != ExportStatus::Finished) {
        std::error_code ec;
        std::filesystem::remove(m_request.file, ec); // never leave a broken file behind
    }
    result.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    spdlog::info("Export {}: {} frames in {:.1f} s ({} / {})",
                 result.status == ExportStatus::Finished ? "finished" : "stopped", result.frames, result.seconds,
                 result.renderer, result.videoEncoder);
    return result;
}

} // namespace loki::exporter
