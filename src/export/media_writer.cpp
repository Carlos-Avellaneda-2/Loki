#include "export/media_writer.h"

#include "export/encoders.h"
#include "media/ffmpeg_utils.h"

extern "C" {
#include <libavutil/audio_fifo.h>
#include <libavutil/opt.h>
}

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <map>
#include <mutex>

namespace loki::exporter {

using namespace loki::media;

namespace {

const char* codecLabel(VideoCodec codec) {
    switch (codec) {
    case VideoCodec::H264: return "H.264";
    case VideoCodec::Hevc: return "HEVC";
    case VideoCodec::ProRes: return "ProRes";
    case VideoCodec::None: return "none";
    }
    return "?";
}

// Picture format we feed the encoder: the first of our preferences it supports.
AVPixelFormat pickPixelFormat(const AVCodec* codec, VideoCodec kind) {
    const void* configs = nullptr;
    int count = 0;
    if (avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0, &configs, &count) < 0 ||
        configs == nullptr) {
        return kind == VideoCodec::ProRes ? AV_PIX_FMT_YUV422P10LE : AV_PIX_FMT_YUV420P;
    }
    const auto* formats = static_cast<const AVPixelFormat*>(configs);
    const std::array preferred = kind == VideoCodec::ProRes
                                     ? std::array{AV_PIX_FMT_YUV422P10LE, AV_PIX_FMT_YUV444P10LE, AV_PIX_FMT_NONE}
                                     : std::array{AV_PIX_FMT_NV12, AV_PIX_FMT_YUV420P, AV_PIX_FMT_NONE};
    for (const auto want : preferred) {
        for (int i = 0; i < count; ++i) {
            if (formats[i] == want) {
                return want;
            }
        }
    }
    return formats[0];
}

void configureVideo(AVCodecContext* c, const AVCodec* codec, const VideoSettings& s) {
    const AVRational fps = toAVRational(s.frameRate);
    c->width = s.width;
    c->height = s.height;
    c->time_base = av_inv_q(fps);
    c->framerate = fps;
    c->pix_fmt = pickPixelFormat(codec, s.codec);
    c->gop_size = s.gop > 0 ? s.gop : std::max(1, static_cast<int>(s.frameRate.toDouble() + 0.5)); // ~1 s
    c->max_b_frames = 0;
    c->color_range = AVCOL_RANGE_MPEG;
    c->colorspace = AVCOL_SPC_BT709;
    c->color_primaries = AVCOL_PRI_BT709;
    c->color_trc = AVCOL_TRC_BT709;
    if (s.codec == VideoCodec::ProRes) {
        av_opt_set_int(c->priv_data, "profile", s.proresProfile, 0);
    } else {
        c->bit_rate = s.bitRate;
        c->rc_max_rate = s.bitRate * 3 / 2;
        c->rc_buffer_size = static_cast<int>(std::min<std::int64_t>(s.bitRate * 2, INT32_MAX));
    }
}

} // namespace

// ---------------------------------------------------------------------------------------------

const std::vector<std::string>& encoderCandidates(VideoCodec codec) {
    static const std::vector<std::string> kH264 = {"h264_nvenc", "h264_qsv", "h264_amf", "libopenh264"};
    static const std::vector<std::string> kHevc = {"hevc_nvenc", "hevc_qsv", "hevc_amf"};
    static const std::vector<std::string> kProRes = {"prores_ks"};
    static const std::vector<std::string> kNone;
    switch (codec) {
    case VideoCodec::H264: return kH264;
    case VideoCodec::Hevc: return kHevc;
    case VideoCodec::ProRes: return kProRes;
    case VideoCodec::None: return kNone;
    }
    return kNone;
}

bool encoderWorks(const std::string& name) {
    const AVCodec* codec = avcodec_find_encoder_by_name(name.c_str());
    if (codec == nullptr) {
        return false;
    }
    VideoSettings s;
    s.width = 640;
    s.height = 360;
    s.bitRate = 2'000'000;
    s.codec = name.starts_with("prores") ? VideoCodec::ProRes : VideoCodec::H264;
    CodecContextPtr ctx(avcodec_alloc_context3(codec));
    configureVideo(ctx.get(), codec, s);
    const int ret = avcodec_open2(ctx.get(), codec, nullptr);
    if (ret < 0) {
        spdlog::info("Encoder {} not usable here: {}", name, ffmpegErrorString(ret));
    }
    return ret >= 0;
}

std::string pickEncoder(VideoCodec codec) {
    static std::mutex mutex;
    static std::map<VideoCodec, std::string> cache;
    std::lock_guard lock(mutex);
    if (const auto it = cache.find(codec); it != cache.end()) {
        return it->second;
    }
    std::string chosen;
    for (const auto& name : encoderCandidates(codec)) {
        if (encoderWorks(name)) {
            chosen = name;
            break;
        }
    }
    spdlog::info("{} export encoder: {}", codecLabel(codec), chosen.empty() ? "none available" : chosen);
    cache.emplace(codec, chosen);
    return chosen;
}

// ---------------------------------------------------------------------------------------------

struct MediaWriter::Impl {
    OutputFormatContextPtr format;
    CodecContextPtr video;
    AVStream* videoStream = nullptr;
    CodecContextPtr audio;
    AVStream* audioStream = nullptr;
    SwsContextPtr sws;
    SwsContextPtr frameSws; // decoded pictures (proxies)
    FramePtr picture;
    FramePtr audioFrame;
    std::unique_ptr<AVAudioFifo, void (*)(AVAudioFifo*)> fifo{nullptr, &av_audio_fifo_free};
    std::int64_t videoPts = 0;
    std::int64_t audioPts = 0;
    int audioFrameSize = 1024;
    int channels = 2;
    std::string encoderName;
    bool finished = false;

    void send(AVCodecContext* ctx, AVStream* stream, const AVFrame* frame) {
        checkFfmpeg(avcodec_send_frame(ctx, frame), "send frame to encoder");
        auto packet = makePacket();
        while (true) {
            const int ret = avcodec_receive_packet(ctx, packet.get());
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                return;
            }
            checkFfmpeg(ret, "encode");
            av_packet_rescale_ts(packet.get(), ctx->time_base, stream->time_base);
            packet->stream_index = stream->index;
            checkFfmpeg(av_interleaved_write_frame(format.get(), packet.get()), "write packet");
        }
    }

    void encodeAudio(bool flushAll) {
        while (av_audio_fifo_size(fifo.get()) >= audioFrameSize ||
               (flushAll && av_audio_fifo_size(fifo.get()) > 0)) {
            const int n = std::min(audioFrameSize, av_audio_fifo_size(fifo.get()));
            av_frame_unref(audioFrame.get());
            audioFrame->format = audio->sample_fmt;
            audioFrame->sample_rate = audio->sample_rate;
            audioFrame->nb_samples = n;
            checkFfmpeg(av_channel_layout_copy(&audioFrame->ch_layout, &audio->ch_layout), "channel layout");
            checkFfmpeg(av_frame_get_buffer(audioFrame.get(), 0), "audio buffer");
            checkFfmpeg(av_audio_fifo_read(fifo.get(), reinterpret_cast<void**>(audioFrame->data), n), "fifo read");
            audioFrame->pts = audioPts;
            audioPts += n;
            send(audio.get(), audioStream, audioFrame.get());
        }
    }
};

MediaWriter::MediaWriter(const std::filesystem::path& file, const VideoSettings& v, const AudioSettings& a)
    : m_impl(std::make_unique<Impl>()) {
    auto& d = *m_impl;
    AVFormatContext* raw = nullptr;
    checkFfmpeg(avformat_alloc_output_context2(&raw, nullptr, nullptr, pathToFfmpeg(file).c_str()),
                "choose container for " + pathToFfmpeg(file));
    d.format.reset(raw);
    const bool globalHeader = (d.format->oformat->flags & AVFMT_GLOBALHEADER) != 0;

    if (v.codec != VideoCodec::None) {
        d.encoderName = v.encoder.empty() ? pickEncoder(v.codec) : v.encoder;
        const AVCodec* codec = d.encoderName.empty() ? nullptr : avcodec_find_encoder_by_name(d.encoderName.c_str());
        if (codec == nullptr) {
            throw MediaError(std::string("No ") + codecLabel(v.codec) + " encoder is available on this computer");
        }
        if (v.width <= 0 || v.height <= 0 || v.width % 2 != 0 || v.height % 2 != 0) {
            throw MediaError("Export size must be positive and even");
        }
        d.video.reset(avcodec_alloc_context3(codec));
        configureVideo(d.video.get(), codec, v);
        if (globalHeader) {
            d.video->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        }
        checkFfmpeg(avcodec_open2(d.video.get(), codec, nullptr), "open video encoder " + d.encoderName);
        d.videoStream = avformat_new_stream(d.format.get(), nullptr);
        checkFfmpeg(avcodec_parameters_from_context(d.videoStream->codecpar, d.video.get()), "video parameters");
        d.videoStream->time_base = d.video->time_base;
        d.videoStream->avg_frame_rate = d.video->framerate;
        d.sws.reset(sws_getContext(v.width, v.height, AV_PIX_FMT_RGBA, v.width, v.height, d.video->pix_fmt,
                                   SWS_BICUBIC, nullptr, nullptr, nullptr));
        if (!d.sws) {
            throw MediaError("Cannot convert pictures for the video encoder");
        }
        // Full-range RGB in, BT.709 limited-range YUV out.
        sws_setColorspaceDetails(d.sws.get(), sws_getCoefficients(SWS_CS_ITU709), 1, sws_getCoefficients(SWS_CS_ITU709),
                                 0, 0, 1 << 16, 1 << 16);
        d.picture = makeFrame();
        d.picture->format = d.video->pix_fmt;
        d.picture->width = v.width;
        d.picture->height = v.height;
        checkFfmpeg(av_frame_get_buffer(d.picture.get(), 0), "picture buffer");
    }

    if (a.codec != AudioCodec::None) {
        const AVCodec* codec = avcodec_find_encoder_by_name(a.codec == AudioCodec::Aac ? "aac" : "pcm_s16le");
        if (codec == nullptr) {
            throw MediaError("Audio encoder not available");
        }
        d.audio.reset(avcodec_alloc_context3(codec));
        d.audio->sample_rate = a.sampleRate;
        d.audio->sample_fmt = a.codec == AudioCodec::Aac ? AV_SAMPLE_FMT_FLTP : AV_SAMPLE_FMT_S16;
        av_channel_layout_default(&d.audio->ch_layout, a.channels);
        d.audio->time_base = AVRational{1, a.sampleRate};
        if (a.codec == AudioCodec::Aac) {
            d.audio->bit_rate = a.bitRate;
        }
        if (globalHeader) {
            d.audio->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        }
        checkFfmpeg(avcodec_open2(d.audio.get(), codec, nullptr), "open audio encoder");
        d.audioStream = avformat_new_stream(d.format.get(), nullptr);
        checkFfmpeg(avcodec_parameters_from_context(d.audioStream->codecpar, d.audio.get()), "audio parameters");
        d.audioStream->time_base = d.audio->time_base;
        d.audioFrameSize = d.audio->frame_size > 0 ? d.audio->frame_size : 1024;
        d.channels = a.channels;
        d.fifo.reset(av_audio_fifo_alloc(d.audio->sample_fmt, a.channels, d.audioFrameSize * 4));
        d.audioFrame = makeFrame();
    }
    if (!d.video && !d.audio) {
        throw MediaError("Nothing to export");
    }
    if ((d.format->oformat->flags & AVFMT_NOFILE) == 0) {
        checkFfmpeg(avio_open(&d.format->pb, pathToFfmpeg(file).c_str(), AVIO_FLAG_WRITE), "create " + pathToFfmpeg(file));
    }
    checkFfmpeg(avformat_write_header(d.format.get(), nullptr), "write header");
}

MediaWriter::~MediaWriter() = default;

std::string MediaWriter::videoEncoderName() const {
    return m_impl->encoderName;
}

void MediaWriter::writeVideo(const std::uint8_t* rgba, int stride) {
    auto& d = *m_impl;
    if (!d.video) {
        return;
    }
    checkFfmpeg(av_frame_make_writable(d.picture.get()), "picture writable");
    const std::uint8_t* src[1] = {rgba};
    const int srcStride[1] = {stride};
    sws_scale(d.sws.get(), src, srcStride, 0, d.picture->height, d.picture->data, d.picture->linesize);
    d.picture->pts = d.videoPts++;
    d.send(d.video.get(), d.videoStream, d.picture.get());
}

void MediaWriter::writeVideoFrame(const media::VideoFrame& frame) {
    auto& d = *m_impl;
    if (!d.video || frame.planeCount == 0) {
        return;
    }
    const AVPixelFormat src = frame.format == media::PixelFormat::Rgba   ? AV_PIX_FMT_RGBA
                              : frame.format == media::PixelFormat::Nv12 ? AV_PIX_FMT_NV12
                                                                         : AV_PIX_FMT_YUV420P;
    d.frameSws.reset(sws_getCachedContext(d.frameSws.release(), frame.width, frame.height, src, d.picture->width,
                                          d.picture->height, d.video->pix_fmt, SWS_BILINEAR, nullptr, nullptr, nullptr));
    if (!d.frameSws) {
        throw MediaError("Cannot convert decoded pictures");
    }
    checkFfmpeg(av_frame_make_writable(d.picture.get()), "picture writable");
    const std::uint8_t* planes[3] = {frame.planes[0].data(), frame.planes[1].data(), frame.planes[2].data()};
    const int strides[3] = {frame.strides[0], frame.strides[1], frame.strides[2]};
    sws_scale(d.frameSws.get(), planes, strides, 0, frame.height, d.picture->data, d.picture->linesize);
    d.picture->pts = d.videoPts++;
    d.send(d.video.get(), d.videoStream, d.picture.get());
}

void MediaWriter::writeAudio(const float* samples, std::size_t frames) {
    auto& d = *m_impl;
    if (!d.audio || frames == 0) {
        return;
    }
    const auto n = static_cast<int>(frames);
    const auto ch = static_cast<std::size_t>(d.channels);
    if (d.audio->sample_fmt == AV_SAMPLE_FMT_FLTP) {
        std::vector<std::vector<float>> planes(ch, std::vector<float>(frames));
        for (std::size_t i = 0; i < frames; ++i) {
            for (std::size_t c = 0; c < ch; ++c) {
                planes[c][i] = samples[i * ch + c];
            }
        }
        std::vector<void*> pointers;
        for (auto& p : planes) {
            pointers.push_back(p.data());
        }
        checkFfmpeg(av_audio_fifo_write(d.fifo.get(), pointers.data(), n), "fifo write");
    } else {
        std::vector<std::int16_t> pcm(frames * ch);
        for (std::size_t i = 0; i < pcm.size(); ++i) {
            pcm[i] = static_cast<std::int16_t>(std::lround(std::clamp(samples[i], -1.0f, 1.0f) * 32767.0f));
        }
        void* data[1] = {pcm.data()};
        checkFfmpeg(av_audio_fifo_write(d.fifo.get(), data, n), "fifo write");
    }
    d.encodeAudio(false);
}

void MediaWriter::finish() {
    auto& d = *m_impl;
    if (d.finished) {
        return;
    }
    d.finished = true;
    if (d.video) {
        d.send(d.video.get(), d.videoStream, nullptr);
    }
    if (d.audio) {
        d.encodeAudio(true);
        d.send(d.audio.get(), d.audioStream, nullptr);
    }
    checkFfmpeg(av_write_trailer(d.format.get()), "write trailer");
}

} // namespace loki::exporter
