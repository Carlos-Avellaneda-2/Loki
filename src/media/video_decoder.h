#pragma once

#include "media/ffmpeg_utils.h"
#include "media/video_frame.h"

#include <filesystem>
#include <deque>
#include <optional>
#include <string>
#include <vector>

namespace loki::media {

enum class HwAccel { None, D3D11VA, Cuda, Dxva2, VideoToolbox, Vaapi };
[[nodiscard]] const char* hwAccelName(HwAccel accel) noexcept;

struct VideoDecoderOptions {
    bool allowHardware = true;
    // Downscale output to fit inside these bounds (0 = keep source size). Used for playback quality.
    int maxWidth = 0;
    int maxHeight = 0;
    // Force RGBA output (thumbnails). Otherwise YUV420P or NV12 is kept for the GPU.
    bool rgbaOutput = false;
    int threads = 0; // 0 = FFmpeg auto
};

struct VideoStreamInfo {
    int width = 0;
    int height = 0;
    Rational frameRate;  // 0 if unknown
    Rational frameDuration;
    Rational duration;
    Rational startTime; // first pts of the stream (seconds)
    std::string codec;
};

// Frame-accurate video decoder for one file. Not thread-safe: use one instance per thread.
// Hardware decoding is tried first (D3D11VA, CUDA/NVDEC, DXVA2 on Windows) and falls back to
// software when unavailable or when it fails; hardware frames are copied back to system memory.
class VideoDecoder {
public:
    VideoDecoder(const std::filesystem::path& path, VideoDecoderOptions options = {});
    ~VideoDecoder();
    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;
    VideoDecoder(VideoDecoder&&) = delete;
    VideoDecoder& operator=(VideoDecoder&&) = delete;

    [[nodiscard]] const VideoStreamInfo& info() const noexcept { return m_info; }
    [[nodiscard]] HwAccel hwAccel() const noexcept { return m_hwAccel; }
    [[nodiscard]] bool isStill() const noexcept { return m_still; }

    // Frame displayed at source time `t` (the last frame with pts <= t). Decodes forward from the
    // current position when `t` is close ahead, otherwise seeks to the previous keyframe first.
    // Returns nullptr past the end of the stream.
    VideoFramePtr frameAt(const Rational& t);

    // Next frame in decode order from the current position; nullptr at end of stream.
    VideoFramePtr nextFrame();

    // Repositions so the next frame returned is the one displayed at `t`.
    void seek(const Rational& t);

    // Decodes every frame from the keyframe before `t` up to (and including) the frame at `t`.
    // Used for reverse playback: one seek yields a whole run of frames.
    std::vector<VideoFramePtr> framesUpTo(const Rational& t);

    // Changes the output size bound (playback quality); takes effect on the next frame.
    void setMaxSize(int maxWidth, int maxHeight);

private:
    bool openCodec(bool hardware);
    bool receiveFrame(AVFrame* out);
    VideoFramePtr convert(AVFrame* frame);
    void seekToKeyframeBefore(const Rational& t);

    std::filesystem::path m_path;
    VideoDecoderOptions m_options;
    FormatContextPtr m_format;
    CodecContextPtr m_codec;
    BufferRefPtr m_hwDevice;
    int m_streamIndex = -1;
    AVRational m_timeBase{0, 1};
    HwAccel m_hwAccel = HwAccel::None;
    int m_hwPixelFormat = -1;
    VideoStreamInfo m_info;
    PacketPtr m_packet;
    FramePtr m_frame;
    FramePtr m_swFrame;
    SwsContextPtr m_sws;
    bool m_eof = false;
    bool m_hwFailed = false;
    bool m_still = false; // single image: every time maps to the one frame
    VideoFramePtr takeOrDecode(); // next frame from the look-ahead queue or the decoder
    VideoFramePtr m_last;               // last frame returned (for frameAt continuity)
    std::deque<VideoFramePtr> m_queue;  // decoded but not yet returned, in presentation order
};

} // namespace loki::media
