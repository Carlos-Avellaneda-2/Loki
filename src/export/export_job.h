#pragma once

#include "export/media_writer.h"
#include "playback/sources.h"
#include "render/cpu_compositor.h"

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class QOffscreenSurface;

namespace loki::exporter {

// --- Presets ----------------------------------------------------------------------------------

struct ExportPreset {
    std::string id;
    std::string name;
    std::string extension; // ".mp4", ".mov", ".wav"
    VideoCodec video = VideoCodec::H264;
    AudioCodec audio = AudioCodec::Aac;
    int width = 0;  // 0 = sequence size
    int height = 0;
    std::int64_t videoBitRate = 0; // 0 = from the output size
    Rational frameRate;            // 0 = sequence frame rate
    int proresProfile = 3;         // ProRes: 1 = LT, 3 = HQ
};
// ProRes LT, video only, sequence size: the render previews (D42).
[[nodiscard]] ExportPreset renderPreviewPreset();
[[nodiscard]] const std::vector<ExportPreset>& exportPresets();
[[nodiscard]] const ExportPreset* findExportPreset(const std::string& id);

// --- Renderers ----------------------------------------------------------------------------------

// Turns a composite into RGBA pixels at the output size. Same composite description as preview
// (D11): the GPU path uses the same GlCompositor as the monitors.
class FrameRenderer {
public:
    virtual ~FrameRenderer() = default;
    FrameRenderer(const FrameRenderer&) = delete;
    FrameRenderer& operator=(const FrameRenderer&) = delete;
    FrameRenderer(FrameRenderer&&) = delete;
    FrameRenderer& operator=(FrameRenderer&&) = delete;

    // Called on the export thread before the first frame; false = this renderer cannot run here.
    virtual bool start() = 0;
    [[nodiscard]] virtual render::CpuImage render(const render::CompositeFrame& frame, int width, int height) = 0;
    // Called on the export thread after the last frame.
    virtual void stop() = 0;
    [[nodiscard]] virtual std::string name() const = 0;

protected:
    FrameRenderer() = default;
};

// Software compositor (D22): works everywhere, slow.
[[nodiscard]] std::unique_ptr<FrameRenderer> makeCpuRenderer();
// OpenGL 3.3 offscreen compositor. `surface` must be created on the GUI thread and outlive the job.
[[nodiscard]] std::unique_ptr<FrameRenderer> makeGlRenderer(QOffscreenSurface* surface);

// --- Job --------------------------------------------------------------------------------------------

struct ExportRequest {
    std::shared_ptr<const playback::SequenceSnapshot> snapshot;
    Rational in;  // sequence range [in, out)
    Rational out;
    std::filesystem::path file;
    ExportPreset preset;
    std::string encoder; // empty = best available
    bool burnCaptions = false; // draw the sequence captions into the picture (D40)
};

enum class ExportStatus { Running, Finished, Cancelled, Failed };

struct ExportResult {
    ExportStatus status = ExportStatus::Running;
    std::string error;
    std::string videoEncoder;
    std::string renderer;
    std::int64_t frames = 0;
    double seconds = 0;
};

// Renders and encodes a sequence range. run() blocks (call it on a worker thread); progress() and
// cancel() may be called from any thread. A cancelled or failed export deletes the partial file.
// `renderers` are tried in order (GPU first, CPU fallback).
class ExportJob {
public:
    ExportJob(ExportRequest request, std::vector<std::unique_ptr<FrameRenderer>> renderers);

    ExportResult run();
    void cancel() noexcept { m_cancel.store(true); }
    [[nodiscard]] double progress() const noexcept { return m_progress.load(); }

    // Output size and frame count for a request (also used by the dialog).
    [[nodiscard]] static std::pair<int, int> outputSize(const ExportRequest& request);
    [[nodiscard]] static std::int64_t frameCount(const ExportRequest& request);
    [[nodiscard]] static Rational frameRate(const ExportRequest& request);

private:
    ExportRequest m_request;
    std::vector<std::unique_ptr<FrameRenderer>> m_renderers;
    std::atomic<bool> m_cancel{false};
    std::atomic<double> m_progress{0.0};
};

} // namespace loki::exporter
