#include "media_generator.h"

#include "media/ffmpeg_utils.h"

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
}

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <numbers>
#include <stdexcept>
#include <vector>

namespace loki::test {

using namespace loki::media;

namespace {

struct Muxer {
    OutputFormatContextPtr ctx;

    explicit Muxer(const std::filesystem::path& file, const char* forcedFormat = nullptr) {
        AVFormatContext* raw = nullptr;
        checkFfmpeg(avformat_alloc_output_context2(&raw, nullptr, forcedFormat, pathToFfmpeg(file).c_str()),
                    "alloc output");
        ctx.reset(raw);
        if ((ctx->oformat->flags & AVFMT_NOFILE) == 0) {
            checkFfmpeg(avio_open(&ctx->pb, pathToFfmpeg(file).c_str(), AVIO_FLAG_WRITE), "avio_open");
        }
    }
};

struct Encoder {
    CodecContextPtr ctx;
    AVStream* stream = nullptr;
};

Encoder addStream(AVFormatContext* fmt, const char* codecName, const std::function<void(AVCodecContext*)>& configure) {
    const AVCodec* codec = avcodec_find_encoder_by_name(codecName);
    if (codec == nullptr) {
        throw std::runtime_error(std::string("Encoder not available: ") + codecName);
    }
    Encoder e;
    e.stream = avformat_new_stream(fmt, nullptr);
    e.ctx.reset(avcodec_alloc_context3(codec));
    configure(e.ctx.get());
    if ((fmt->oformat->flags & AVFMT_GLOBALHEADER) != 0) {
        e.ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    checkFfmpeg(avcodec_open2(e.ctx.get(), codec, nullptr), std::string("open encoder ") + codecName);
    checkFfmpeg(avcodec_parameters_from_context(e.stream->codecpar, e.ctx.get()), "parameters_from_context");
    e.stream->time_base = e.ctx->time_base;
    return e;
}

void drain(AVFormatContext* fmt, Encoder& e, AVFrame* frame) {
    checkFfmpeg(avcodec_send_frame(e.ctx.get(), frame), "send_frame");
    auto packet = makePacket();
    while (true) {
        const int ret = avcodec_receive_packet(e.ctx.get(), packet.get());
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            return;
        }
        checkFfmpeg(ret, "receive_packet");
        if (packet->duration == 0 && e.ctx->codec_type == AVMEDIA_TYPE_VIDEO) {
            packet->duration = 1; // one frame in the 1/fps time base: keeps the last frame in the edit list
        }
        av_packet_rescale_ts(packet.get(), e.ctx->time_base, e.stream->time_base);
        packet->stream_index = e.stream->index;
        checkFfmpeg(av_interleaved_write_frame(fmt, packet.get()), "write_frame");
    }
}

} // namespace

void generateVideo(const std::filesystem::path& file, const VideoSpec& spec) {
    Muxer mux(file);
    const AVRational fps = toAVRational(spec.frameRate);
    auto video = addStream(mux.ctx.get(), spec.codec.c_str(), [&](AVCodecContext* c) {
        c->width = spec.width;
        c->height = spec.height;
        c->pix_fmt = AV_PIX_FMT_YUV420P;
        c->time_base = av_inv_q(fps);
        c->framerate = fps;
        c->gop_size = spec.gop;
        c->max_b_frames = spec.codec == "mpeg4" ? spec.bFrames : 0;
        c->bit_rate = 2'000'000;
        c->color_range = AVCOL_RANGE_MPEG;
        c->colorspace = AVCOL_SPC_BT709;
    });

    Encoder audio;
    if (spec.withAudio) {
        audio = addStream(mux.ctx.get(), "aac", [](AVCodecContext* c) {
            c->sample_rate = 48000;
            c->sample_fmt = AV_SAMPLE_FMT_FLTP;
            av_channel_layout_default(&c->ch_layout, 2);
            c->time_base = AVRational{1, 48000};
            c->bit_rate = 128000;
        });
    }
    checkFfmpeg(avformat_write_header(mux.ctx.get(), nullptr), "write_header");

    auto frame = makeFrame();
    frame->format = AV_PIX_FMT_YUV420P;
    frame->width = spec.width;
    frame->height = spec.height;
    checkFfmpeg(av_frame_get_buffer(frame.get(), 0), "frame buffer");

    auto audioFrame = makeFrame();
    std::int64_t audioPts = 0;
    const Rational videoEnd = framesToTime(spec.frames, spec.frameRate);

    for (int i = 0; i < spec.frames; ++i) {
        checkFfmpeg(av_frame_make_writable(frame.get()), "make_writable");
        const int luma = expectedLuma(i);
        for (int y = 0; y < spec.height; ++y) {
            std::memset(frame->data[0] + y * frame->linesize[0], luma, static_cast<std::size_t>(spec.width));
        }
        for (int y = 0; y < (spec.height + 1) / 2; ++y) {
            std::memset(frame->data[1] + y * frame->linesize[1], 128, static_cast<std::size_t>((spec.width + 1) / 2));
            std::memset(frame->data[2] + y * frame->linesize[2], 128, static_cast<std::size_t>((spec.width + 1) / 2));
        }
        frame->pts = i;
        drain(mux.ctx.get(), video, frame.get());

        // Keep audio roughly interleaved with video.
        while (spec.withAudio && Rational(audioPts, 48000) < min(framesToTime(i + 1, spec.frameRate), videoEnd)) {
            av_frame_unref(audioFrame.get());
            audioFrame->format = AV_SAMPLE_FMT_FLTP;
            audioFrame->sample_rate = 48000;
            audioFrame->nb_samples = audio.ctx->frame_size;
            av_channel_layout_default(&audioFrame->ch_layout, 2);
            checkFfmpeg(av_frame_get_buffer(audioFrame.get(), 0), "audio buffer");
            for (int c = 0; c < 2; ++c) {
                auto* samples = reinterpret_cast<float*>(audioFrame->data[c]);
                for (int s = 0; s < audioFrame->nb_samples; ++s) {
                    const double t = static_cast<double>(audioPts + s) / 48000.0;
                    samples[s] = static_cast<float>(0.5 * std::sin(2.0 * std::numbers::pi * 440.0 * t));
                }
            }
            audioFrame->pts = audioPts;
            audioPts += audioFrame->nb_samples;
            drain(mux.ctx.get(), audio, audioFrame.get());
        }
    }
    drain(mux.ctx.get(), video, nullptr);
    if (spec.withAudio) {
        drain(mux.ctx.get(), audio, nullptr);
    }
    checkFfmpeg(av_write_trailer(mux.ctx.get()), "write_trailer");
}

void generateWav(const std::filesystem::path& file, double seconds, int sampleRate, int channels, double frequency) {
    Muxer mux(file);
    auto audio = addStream(mux.ctx.get(), "pcm_s16le", [&](AVCodecContext* c) {
        c->sample_rate = sampleRate;
        c->sample_fmt = AV_SAMPLE_FMT_S16;
        av_channel_layout_default(&c->ch_layout, channels);
        c->time_base = AVRational{1, sampleRate};
    });
    checkFfmpeg(avformat_write_header(mux.ctx.get(), nullptr), "write_header");
    const auto total = static_cast<std::int64_t>(std::llround(seconds * sampleRate));
    auto frame = makeFrame();
    constexpr int kBlock = 1024;
    for (std::int64_t pos = 0; pos < total; pos += kBlock) {
        av_frame_unref(frame.get());
        frame->format = AV_SAMPLE_FMT_S16;
        frame->sample_rate = sampleRate;
        frame->nb_samples = static_cast<int>(std::min<std::int64_t>(kBlock, total - pos));
        av_channel_layout_default(&frame->ch_layout, channels);
        checkFfmpeg(av_frame_get_buffer(frame.get(), 0), "buffer");
        auto* samples = reinterpret_cast<std::int16_t*>(frame->data[0]);
        for (int s = 0; s < frame->nb_samples; ++s) {
            const double t = static_cast<double>(pos + s) / sampleRate;
            const auto v = static_cast<std::int16_t>(std::lround(0.5 * 32767.0 * std::sin(2.0 * std::numbers::pi * frequency * t)));
            for (int c = 0; c < channels; ++c) {
                samples[s * channels + c] = v;
            }
        }
        frame->pts = pos;
        drain(mux.ctx.get(), audio, frame.get());
    }
    drain(mux.ctx.get(), audio, nullptr);
    checkFfmpeg(av_write_trailer(mux.ctx.get()), "write_trailer");
}

void generatePng(const std::filesystem::path& file, int width, int height) {
    Muxer mux(file, "image2");
    auto png = addStream(mux.ctx.get(), "png", [&](AVCodecContext* c) {
        c->width = width;
        c->height = height;
        c->pix_fmt = AV_PIX_FMT_RGB24;
        c->time_base = AVRational{1, 25};
    });
    AVDictionary* options = nullptr;
    av_dict_set(&options, "update", "1", 0); // single image, no sequence pattern
    const int written = avformat_write_header(mux.ctx.get(), &options);
    av_dict_free(&options);
    checkFfmpeg(written, "write_header");
    auto frame = makeFrame();
    frame->format = AV_PIX_FMT_RGB24;
    frame->width = width;
    frame->height = height;
    checkFfmpeg(av_frame_get_buffer(frame.get(), 0), "buffer");
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            auto* p = frame->data[0] + y * frame->linesize[0] + x * 3;
            p[0] = static_cast<std::uint8_t>(x * 255 / std::max(1, width - 1));
            p[1] = static_cast<std::uint8_t>(y * 255 / std::max(1, height - 1));
            p[2] = 128;
        }
    }
    frame->pts = 0;
    drain(mux.ctx.get(), png, frame.get());
    drain(mux.ctx.get(), png, nullptr);
    checkFfmpeg(av_write_trailer(mux.ctx.get()), "write_trailer");
}

} // namespace loki::test
