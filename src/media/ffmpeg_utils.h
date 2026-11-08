#pragma once

#include "util/rational.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/hwcontext.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>

namespace loki::media {

// Any failure opening, probing or decoding media.
class MediaError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// RAII owners for FFmpeg objects.
struct FormatContextDeleter {
    void operator()(AVFormatContext* p) const noexcept { avformat_close_input(&p); }
};
struct OutputFormatContextDeleter {
    void operator()(AVFormatContext* p) const noexcept {
        if (p != nullptr) {
            if (p->pb != nullptr && (p->oformat->flags & AVFMT_NOFILE) == 0) {
                avio_closep(&p->pb);
            }
            avformat_free_context(p);
        }
    }
};
struct CodecContextDeleter {
    void operator()(AVCodecContext* p) const noexcept { avcodec_free_context(&p); }
};
struct FrameDeleter {
    void operator()(AVFrame* p) const noexcept { av_frame_free(&p); }
};
struct PacketDeleter {
    void operator()(AVPacket* p) const noexcept { av_packet_free(&p); }
};
struct BufferRefDeleter {
    void operator()(AVBufferRef* p) const noexcept { av_buffer_unref(&p); }
};
struct SwsContextDeleter {
    void operator()(SwsContext* p) const noexcept { sws_freeContext(p); }
};
struct SwrContextDeleter {
    void operator()(SwrContext* p) const noexcept { swr_free(&p); }
};

using FormatContextPtr = std::unique_ptr<AVFormatContext, FormatContextDeleter>;
using OutputFormatContextPtr = std::unique_ptr<AVFormatContext, OutputFormatContextDeleter>;
using CodecContextPtr = std::unique_ptr<AVCodecContext, CodecContextDeleter>;
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;
using BufferRefPtr = std::unique_ptr<AVBufferRef, BufferRefDeleter>;
using SwsContextPtr = std::unique_ptr<SwsContext, SwsContextDeleter>;
using SwrContextPtr = std::unique_ptr<SwrContext, SwrContextDeleter>;

[[nodiscard]] FramePtr makeFrame();
[[nodiscard]] PacketPtr makePacket();

// Human-readable text for an FFmpeg error code.
[[nodiscard]] std::string ffmpegErrorString(int error);
// Throws MediaError("<what>: <ffmpeg message>") when error < 0; returns error otherwise.
int checkFfmpeg(int error, const std::string& what);

// Opens a file for demuxing and reads stream info. Paths are passed to FFmpeg as UTF-8.
[[nodiscard]] FormatContextPtr openInput(const std::filesystem::path& path);
[[nodiscard]] std::string pathToFfmpeg(const std::filesystem::path& path);

[[nodiscard]] Rational toRational(AVRational r);         // 0/1 when r.den == 0
[[nodiscard]] AVRational toAVRational(const Rational& r); // throws if it does not fit in int
// Converts a timestamp in `timeBase` units to seconds (exact).
[[nodiscard]] Rational timestampToTime(std::int64_t ts, AVRational timeBase);
// Converts seconds to a timestamp in `timeBase` units (rounded down).
[[nodiscard]] std::int64_t timeToTimestamp(const Rational& time, AVRational timeBase);

// Picks a stream's frame rate: r_frame_rate (container base rate) unless it looks like a field
// rate, then snaps to the nearest standard rate within 0.1% (23.976, 24, 25, 29.97, 30, 48, 50,
// 59.94, 60, 120). avg_frame_rate alone is unreliable with B-frames and edit lists. 0 if unknown.
[[nodiscard]] Rational streamFrameRate(const AVStream* stream);

// Routes FFmpeg's own log output into spdlog (call once at startup; idempotent).
void installFfmpegLogBridge();

} // namespace loki::media
