#include "media/video_decoder.h"

#include "model/project_json.h"

extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
}

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <cstdint>

namespace loki::media {

namespace {

// Beyond this distance ahead, seeking is cheaper than decoding forward.
const Rational kForwardDecodeWindow(2);

struct HwCandidate {
    HwAccel accel;
    AVHWDeviceType type;
};

std::vector<HwCandidate> platformHwCandidates() {
#if defined(_WIN32)
    return {{HwAccel::D3D11VA, AV_HWDEVICE_TYPE_D3D11VA},
            {HwAccel::Cuda, AV_HWDEVICE_TYPE_CUDA},
            {HwAccel::Dxva2, AV_HWDEVICE_TYPE_DXVA2}};
#elif defined(__APPLE__)
    return {{HwAccel::VideoToolbox, AV_HWDEVICE_TYPE_VIDEOTOOLBOX}};
#else
    return {{HwAccel::Vaapi, AV_HWDEVICE_TYPE_VAAPI}, {HwAccel::Cuda, AV_HWDEVICE_TYPE_CUDA}};
#endif
}

AVPixelFormat getHwFormat(AVCodecContext* ctx, const AVPixelFormat* formats) {
    const auto wanted = static_cast<AVPixelFormat>(reinterpret_cast<std::intptr_t>(ctx->opaque));
    for (const auto* p = formats; *p != AV_PIX_FMT_NONE; ++p) {
        if (*p == wanted) {
            return *p;
        }
    }
    // Hardware format not offered for this stream (e.g. unsupported profile): pick software.
    for (const auto* p = formats; *p != AV_PIX_FMT_NONE; ++p) {
        const auto* desc = av_pix_fmt_desc_get(*p);
        if (desc != nullptr && (desc->flags & AV_PIX_FMT_FLAG_HWACCEL) == 0) {
            return *p;
        }
    }
    return AV_PIX_FMT_NONE;
}

ColorMatrix matrixOf(const AVFrame* f) {
    switch (f->colorspace) {
    case AVCOL_SPC_BT709: return ColorMatrix::Bt709;
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M: return ColorMatrix::Bt601;
    default: return f->height >= 720 ? ColorMatrix::Bt709 : ColorMatrix::Bt601;
    }
}

ColorRange rangeOf(const AVFrame* f) {
    if (f->color_range == AVCOL_RANGE_JPEG) {
        return ColorRange::Full;
    }
    switch (static_cast<AVPixelFormat>(f->format)) {
    case AV_PIX_FMT_YUVJ420P:
    case AV_PIX_FMT_YUVJ422P:
    case AV_PIX_FMT_YUVJ444P: return ColorRange::Full;
    default: return ColorRange::Limited;
    }
}

void allocatePlanes(VideoFrame& f) {
    const int w = f.width;
    const int h = f.height;
    const int cw = (w + 1) / 2;
    const int ch = (h + 1) / 2;
    switch (f.format) {
    case PixelFormat::Yuv420P:
        f.planeCount = 3;
        f.strides = {w, cw, cw};
        f.planeHeights = {h, ch, ch};
        break;
    case PixelFormat::Nv12:
        f.planeCount = 2;
        f.strides = {w, cw * 2, 0};
        f.planeHeights = {h, ch, 0};
        break;
    case PixelFormat::Rgba:
        f.planeCount = 1;
        f.strides = {w * 4, 0, 0};
        f.planeHeights = {h, 0, 0};
        break;
    }
    for (int i = 0; i < f.planeCount; ++i) {
        f.planes[static_cast<std::size_t>(i)].resize(static_cast<std::size_t>(f.strides[static_cast<std::size_t>(i)]) *
                                                     static_cast<std::size_t>(f.planeHeights[static_cast<std::size_t>(i)]));
    }
}

AVPixelFormat toAv(PixelFormat f) {
    switch (f) {
    case PixelFormat::Yuv420P: return AV_PIX_FMT_YUV420P;
    case PixelFormat::Nv12: return AV_PIX_FMT_NV12;
    case PixelFormat::Rgba: return AV_PIX_FMT_RGBA;
    }
    return AV_PIX_FMT_YUV420P;
}

} // namespace

const char* hwAccelName(HwAccel accel) noexcept {
    switch (accel) {
    case HwAccel::None: return "software";
    case HwAccel::D3D11VA: return "d3d11va";
    case HwAccel::Cuda: return "cuda";
    case HwAccel::Dxva2: return "dxva2";
    case HwAccel::VideoToolbox: return "videotoolbox";
    case HwAccel::Vaapi: return "vaapi";
    }
    return "unknown";
}

VideoDecoder::VideoDecoder(const std::filesystem::path& path, VideoDecoderOptions options)
    : m_path(path), m_options(options), m_format(openInput(path)), m_packet(makePacket()), m_frame(makeFrame()),
      m_swFrame(makeFrame()) {
    for (unsigned i = 0; i < m_format->nb_streams; ++i) {
        const auto* s = m_format->streams[i];
        if (s->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && (s->disposition & AV_DISPOSITION_ATTACHED_PIC) == 0) {
            m_streamIndex = static_cast<int>(i);
            break;
        }
    }
    if (m_streamIndex < 0) {
        throw MediaError("No video stream in " + model::pathToUtf8(path));
    }
    // Keep the demuxer from reading other streams' packets into memory.
    for (unsigned i = 0; i < m_format->nb_streams; ++i) {
        if (static_cast<int>(i) != m_streamIndex) {
            m_format->streams[i]->discard = AVDISCARD_ALL;
        }
    }

    const auto* stream = m_format->streams[m_streamIndex];
    m_timeBase = stream->time_base;
    m_info.width = stream->codecpar->width;
    m_info.height = stream->codecpar->height;
    m_info.frameRate = streamFrameRate(stream);
    {
        const std::string demuxer = m_format->iformat != nullptr ? m_format->iformat->name : "";
        m_still = demuxer == "image2" || demuxer.find("_pipe") != std::string::npos || stream->nb_frames == 1;
    }
    m_info.frameDuration = m_info.frameRate.isPositive() ? m_info.frameRate.reciprocal() : Rational();
    m_info.startTime = stream->start_time != AV_NOPTS_VALUE ? timestampToTime(stream->start_time, m_timeBase) : Rational();
    if (stream->duration != AV_NOPTS_VALUE && stream->duration > 0) {
        m_info.duration = timestampToTime(stream->duration, m_timeBase);
    } else if (m_format->duration > 0) {
        m_info.duration = Rational(m_format->duration, AV_TIME_BASE);
    }
    const auto* desc = avcodec_descriptor_get(stream->codecpar->codec_id);
    m_info.codec = desc != nullptr ? desc->name : "unknown";

    bool opened = m_options.allowHardware && openCodec(true);
    if (opened) {
        // Smoke-test the hardware path on the first frame; drivers reject some profiles late.
        bool ok = false;
        try {
            ok = receiveFrame(m_frame.get()) && m_frame->format == m_hwPixelFormat;
        } catch (const MediaError& e) {
            spdlog::warn("Hardware decoding ({}) failed for {}: {}", hwAccelName(m_hwAccel), model::pathToUtf8(path),
                         e.what());
        }
        av_frame_unref(m_frame.get());
        if (!ok) {
            opened = false;
        }
    }
    if (!opened) {
        m_hwDevice.reset();
        m_hwAccel = HwAccel::None;
        if (!openCodec(false)) {
            throw MediaError("Cannot open video decoder for " + model::pathToUtf8(path));
        }
    }
    seekToKeyframeBefore(Rational());
    spdlog::debug("VideoDecoder {} {}x{} {} via {}", model::pathToUtf8(path), m_info.width, m_info.height, m_info.codec,
                  hwAccelName(m_hwAccel));
}

VideoDecoder::~VideoDecoder() = default;

bool VideoDecoder::openCodec(bool hardware) {
    const auto* stream = m_format->streams[m_streamIndex];
    const AVCodec* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
    if (decoder == nullptr) {
        throw MediaError("No decoder for codec " + m_info.codec);
    }
    m_codec.reset(avcodec_alloc_context3(decoder));
    if (!m_codec) {
        throw MediaError("avcodec_alloc_context3 failed");
    }
    checkFfmpeg(avcodec_parameters_to_context(m_codec.get(), stream->codecpar), "avcodec_parameters_to_context");
    m_codec->pkt_timebase = stream->time_base;
    m_codec->thread_count = m_options.threads;
    m_hwAccel = HwAccel::None;
    m_hwPixelFormat = -1;

    if (hardware) {
        bool configured = false;
        for (const auto& candidate : platformHwCandidates()) {
            AVPixelFormat pixFmt = AV_PIX_FMT_NONE;
            for (int i = 0;; ++i) {
                const AVCodecHWConfig* cfg = avcodec_get_hw_config(decoder, i);
                if (cfg == nullptr) {
                    break;
                }
                if ((cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) != 0 && cfg->device_type == candidate.type) {
                    pixFmt = cfg->pix_fmt;
                    break;
                }
            }
            if (pixFmt == AV_PIX_FMT_NONE) {
                continue;
            }
            AVBufferRef* device = nullptr;
            if (av_hwdevice_ctx_create(&device, candidate.type, nullptr, nullptr, 0) < 0) {
                continue;
            }
            m_hwDevice.reset(device);
            m_codec->hw_device_ctx = av_buffer_ref(device);
            m_codec->opaque = reinterpret_cast<void*>(static_cast<std::intptr_t>(pixFmt));
            m_codec->get_format = &getHwFormat;
            m_hwAccel = candidate.accel;
            m_hwPixelFormat = pixFmt;
            configured = true;
            break;
        }
        if (!configured) {
            return false;
        }
    }

    if (avcodec_open2(m_codec.get(), decoder, nullptr) < 0) {
        m_codec.reset();
        return false;
    }
    m_eof = false;
    return true;
}

bool VideoDecoder::receiveFrame(AVFrame* out) {
    bool draining = false;
    while (true) {
        const int ret = avcodec_receive_frame(m_codec.get(), out);
        if (ret == 0) {
            return true;
        }
        if (ret == AVERROR_EOF) {
            m_eof = true;
            return false;
        }
        if (ret != AVERROR(EAGAIN)) {
            checkFfmpeg(ret, "Decoding " + model::pathToUtf8(m_path));
        }
        if (draining) {
            continue;
        }
        // Feed the next packet of our stream.
        while (true) {
            const int r = av_read_frame(m_format.get(), m_packet.get());
            if (r == AVERROR_EOF) {
                avcodec_send_packet(m_codec.get(), nullptr); // enter draining mode
                draining = true;
                break;
            }
            checkFfmpeg(r, "Reading " + model::pathToUtf8(m_path));
            if (m_packet->stream_index != m_streamIndex) {
                av_packet_unref(m_packet.get());
                continue;
            }
            const int s = avcodec_send_packet(m_codec.get(), m_packet.get());
            av_packet_unref(m_packet.get());
            if (s < 0 && s != AVERROR(EAGAIN) && s != AVERROR_INVALIDDATA) {
                checkFfmpeg(s, "Sending packet of " + model::pathToUtf8(m_path));
            }
            break;
        }
    }
}

VideoFramePtr VideoDecoder::convert(AVFrame* decoded) {
    AVFrame* src = decoded;
    if (m_hwPixelFormat >= 0 && decoded->format == m_hwPixelFormat) {
        av_frame_unref(m_swFrame.get());
        checkFfmpeg(av_hwframe_transfer_data(m_swFrame.get(), decoded, 0), "Downloading hardware frame");
        checkFfmpeg(av_frame_copy_props(m_swFrame.get(), decoded), "Copying frame properties");
        src = m_swFrame.get();
    }

    auto frame = std::make_shared<VideoFrame>();
    const std::int64_t ts = decoded->best_effort_timestamp != AV_NOPTS_VALUE ? decoded->best_effort_timestamp : decoded->pts;
    if (ts != AV_NOPTS_VALUE) {
        frame->pts = timestampToTime(ts, m_timeBase) - m_info.startTime;
    } else if (m_last) {
        frame->pts = m_last->pts + m_last->duration;
    }
    frame->duration = decoded->duration > 0 ? timestampToTime(decoded->duration, m_timeBase) : m_info.frameDuration;
    frame->matrix = matrixOf(src);
    frame->range = rangeOf(src);

    // Output size: fit inside the bounds, keep aspect, even dimensions for chroma subsampling.
    int w = src->width;
    int h = src->height;
    if ((m_options.maxWidth > 0 && w > m_options.maxWidth) || (m_options.maxHeight > 0 && h > m_options.maxHeight)) {
        const double sx = m_options.maxWidth > 0 ? static_cast<double>(m_options.maxWidth) / w : 1e9;
        const double sy = m_options.maxHeight > 0 ? static_cast<double>(m_options.maxHeight) / h : 1e9;
        const double s = std::min(sx, sy);
        w = std::max(2, static_cast<int>(w * s) & ~1);
        h = std::max(2, static_cast<int>(h * s) & ~1);
    }
    const auto srcFormat = static_cast<AVPixelFormat>(src->format);
    if (m_options.rgbaOutput) {
        frame->format = PixelFormat::Rgba;
    } else if (srcFormat == AV_PIX_FMT_NV12 && w == src->width && h == src->height) {
        frame->format = PixelFormat::Nv12;
    } else {
        frame->format = PixelFormat::Yuv420P;
    }
    frame->width = w;
    frame->height = h;
    allocatePlanes(*frame);

    std::array<std::uint8_t*, 4> dst{};
    std::array<int, 4> dstStride{};
    for (int i = 0; i < frame->planeCount; ++i) {
        dst[static_cast<std::size_t>(i)] = frame->planes[static_cast<std::size_t>(i)].data();
        dstStride[static_cast<std::size_t>(i)] = frame->strides[static_cast<std::size_t>(i)];
    }

    const auto dstFormat = toAv(frame->format);
    if (srcFormat == dstFormat && w == src->width && h == src->height) {
        av_image_copy(dst.data(), dstStride.data(), const_cast<const std::uint8_t**>(src->data), src->linesize,
                      dstFormat, w, h);
    } else {
        m_sws.reset(sws_getCachedContext(m_sws.release(), src->width, src->height, srcFormat, w, h, dstFormat,
                                         SWS_BILINEAR, nullptr, nullptr, nullptr));
        if (!m_sws) {
            throw MediaError("Cannot convert pixel format of " + model::pathToUtf8(m_path));
        }
        sws_scale(m_sws.get(), src->data, src->linesize, 0, src->height, dst.data(), dstStride.data());
        if (frame->format != PixelFormat::Rgba) {
            frame->range = ColorRange::Limited; // swscale outputs limited-range YUV
        }
    }
    av_frame_unref(m_swFrame.get());
    return frame;
}

void VideoDecoder::seekToKeyframeBefore(const Rational& t) {
    const Rational target = max(Rational(), t) + m_info.startTime;
    const std::int64_t ts = timeToTimestamp(target, m_timeBase);
    if (av_seek_frame(m_format.get(), m_streamIndex, ts, AVSEEK_FLAG_BACKWARD) < 0) {
        // Some demuxers only seek with the generic API.
        av_seek_frame(m_format.get(), -1, av_rescale_q(ts, m_timeBase, AVRational{1, AV_TIME_BASE}),
                      AVSEEK_FLAG_BACKWARD);
    }
    avcodec_flush_buffers(m_codec.get());
    m_eof = false;
    m_last.reset();
    m_queue.clear();
}

VideoFramePtr VideoDecoder::takeOrDecode() {
    if (!m_queue.empty()) {
        auto f = std::move(m_queue.front());
        m_queue.pop_front();
        return f;
    }
    if (m_eof || !receiveFrame(m_frame.get())) {
        return nullptr;
    }
    auto converted = convert(m_frame.get());
    av_frame_unref(m_frame.get());
    return converted;
}

VideoFramePtr VideoDecoder::nextFrame() {
    auto f = takeOrDecode();
    if (f) {
        m_last = f;
    }
    return f;
}

VideoFramePtr VideoDecoder::frameAt(const Rational& requested) {
    if (m_still) {
        // A still image shows its only frame at every time.
        if (!m_last) {
            seekToKeyframeBefore(Rational());
            nextFrame();
        }
        return m_last;
    }
    const Rational t = max(Rational(), requested);
    if (m_last && m_last->contains(t)) {
        return m_last;
    }
    const Rational from = m_last ? m_last->pts : (!m_queue.empty() ? m_queue.front()->pts : Rational(-1));
    const bool canContinue = !from.isNegative() && from <= t && t - from < kForwardDecodeWindow;
    if (!canContinue) {
        seekToKeyframeBefore(t);
    }

    for (int attempt = 0; attempt < 4; ++attempt) {
        VideoFramePtr previous = m_last;
        while (true) {
            VideoFramePtr candidate = takeOrDecode();
            if (!candidate) { // end of stream
                const Rational lastDuration =
                    previous && previous->duration.isPositive() ? previous->duration : m_info.frameDuration;
                if (previous && t < previous->pts + lastDuration) {
                    m_last = previous;
                    return previous;
                }
                return nullptr;
            }
            if (candidate->pts > t) {
                m_queue.push_front(std::move(candidate)); // keep it for the next request
                if (previous) {
                    m_last = previous;
                    return previous;
                }
                break; // landed after the target: seek further back and retry
            }
            previous = candidate;
            m_last = candidate;
            if (candidate->contains(t)) {
                return candidate;
            }
        }
        if (t.isZero() || attempt == 3) {
            // Target precedes the first decodable frame: show the first frame.
            seekToKeyframeBefore(Rational());
            return nextFrame();
        }
        seekToKeyframeBefore(t - Rational(1LL << attempt));
    }
    return nullptr;
}

void VideoDecoder::seek(const Rational& t) {
    if (auto frame = frameAt(t)) {
        m_queue.push_front(std::move(frame)); // next nextFrame() returns the frame at t
    }
    m_last.reset();
}

std::vector<VideoFramePtr> VideoDecoder::framesUpTo(const Rational& t) {
    std::vector<VideoFramePtr> frames;
    seekToKeyframeBefore(t);
    for (int attempt = 0; attempt < 4; ++attempt) {
        frames.clear();
        while (auto f = nextFrame()) {
            if (f->pts > t) {
                m_queue.push_front(std::move(f));
                break;
            }
            frames.push_back(std::move(f));
        }
        if (!frames.empty() || t.isZero()) {
            break;
        }
        seekToKeyframeBefore(t - Rational(1LL << attempt));
    }
    if (!frames.empty()) {
        m_last = frames.back();
    }
    return frames;
}

void VideoDecoder::setMaxSize(int maxWidth, int maxHeight) {
    if (maxWidth == m_options.maxWidth && maxHeight == m_options.maxHeight) {
        return;
    }
    m_options.maxWidth = maxWidth;
    m_options.maxHeight = maxHeight;
    // Frames already converted have the old size: forget them so the next request re-decodes.
    m_last.reset();
    m_queue.clear();
}

} // namespace loki::media
