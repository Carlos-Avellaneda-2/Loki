#include "media/audio_decoder.h"

#include "model/project_json.h"

#include <algorithm>
#include <cstring>

namespace loki::media {

AudioDecoder::AudioDecoder(const std::filesystem::path& path, int sampleRate, int channels)
    : m_format(openInput(path)), m_packet(makePacket()), m_frame(makeFrame()), m_outRate(sampleRate),
      m_outChannels(channels) {
    m_streamIndex = av_find_best_stream(m_format.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (m_streamIndex < 0) {
        throw MediaError("No audio stream in " + model::pathToUtf8(path));
    }
    for (unsigned i = 0; i < m_format->nb_streams; ++i) {
        if (static_cast<int>(i) != m_streamIndex) {
            m_format->streams[i]->discard = AVDISCARD_ALL;
        }
    }
    const auto* stream = m_format->streams[m_streamIndex];
    m_timeBase = stream->time_base;
    m_startTime = stream->start_time != AV_NOPTS_VALUE ? timestampToTime(stream->start_time, m_timeBase) : Rational();

    const AVCodec* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
    if (decoder == nullptr) {
        throw MediaError("No audio decoder for " + model::pathToUtf8(path));
    }
    m_codec.reset(avcodec_alloc_context3(decoder));
    checkFfmpeg(avcodec_parameters_to_context(m_codec.get(), stream->codecpar), "avcodec_parameters_to_context");
    m_codec->pkt_timebase = stream->time_base;
    checkFfmpeg(avcodec_open2(m_codec.get(), decoder, nullptr), "Opening audio decoder");

    m_info.sourceSampleRate = m_codec->sample_rate;
    m_info.sourceChannels = m_codec->ch_layout.nb_channels;
    if (m_outChannels <= 0) {
        m_outChannels = std::max(1, m_info.sourceChannels);
    }
    const auto* desc = avcodec_descriptor_get(stream->codecpar->codec_id);
    m_info.codec = desc != nullptr ? desc->name : "unknown";
    if (stream->duration != AV_NOPTS_VALUE && stream->duration > 0) {
        m_info.duration = timestampToTime(stream->duration, m_timeBase);
    } else if (m_format->duration > 0) {
        m_info.duration = Rational(m_format->duration, AV_TIME_BASE);
    }
    resetResampler();
    seek(Rational());
}

AudioDecoder::~AudioDecoder() = default;

void AudioDecoder::resetResampler() {
    AVChannelLayout outLayout;
    av_channel_layout_default(&outLayout, m_outChannels);
    SwrContext* raw = nullptr;
    checkFfmpeg(swr_alloc_set_opts2(&raw, &outLayout, AV_SAMPLE_FMT_FLT, m_outRate, &m_codec->ch_layout,
                                    m_codec->sample_fmt, m_codec->sample_rate, 0, nullptr),
                "swr_alloc_set_opts2");
    m_swr.reset(raw);
    checkFfmpeg(swr_init(m_swr.get()), "swr_init");
    av_channel_layout_uninit(&outLayout);
}

void AudioDecoder::seek(const Rational& t) {
    const Rational target = max(Rational(), t);
    m_seekTarget = timeToFrames(target, Rational(m_outRate), Rounding::Nearest);
    m_position = m_seekTarget;
    // Seek a little earlier: some codecs need pre-roll and frame boundaries rarely match.
    const Rational seekTo = max(Rational(), target - Rational(1, 10)) + m_startTime;
    av_seek_frame(m_format.get(), m_streamIndex, timeToTimestamp(seekTo, m_timeBase), AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(m_codec.get());
    resetResampler();
    m_buffer.clear();
    m_bufferOffset = 0;
    m_decodedEnd = -1;
    m_eof = false;
    m_draining = false;
}

bool AudioDecoder::decodeMore() {
    while (true) {
        const int ret = avcodec_receive_frame(m_codec.get(), m_frame.get());
        if (ret == 0) {
            break;
        }
        if (ret == AVERROR_EOF) {
            m_eof = true;
            return false;
        }
        if (ret != AVERROR(EAGAIN)) {
            checkFfmpeg(ret, "Decoding audio");
        }
        if (m_draining) {
            continue;
        }
        const int r = av_read_frame(m_format.get(), m_packet.get());
        if (r == AVERROR_EOF) {
            avcodec_send_packet(m_codec.get(), nullptr);
            m_draining = true;
            continue;
        }
        checkFfmpeg(r, "Reading audio");
        if (m_packet->stream_index == m_streamIndex) {
            const int s = avcodec_send_packet(m_codec.get(), m_packet.get());
            if (s < 0 && s != AVERROR(EAGAIN) && s != AVERROR_INVALIDDATA) {
                av_packet_unref(m_packet.get());
                checkFfmpeg(s, "Sending audio packet");
            }
        }
        av_packet_unref(m_packet.get());
    }

    // Timestamp of this frame in output frames (only needed for the first frame after a seek).
    if (m_decodedEnd < 0) {
        const std::int64_t ts =
            m_frame->best_effort_timestamp != AV_NOPTS_VALUE ? m_frame->best_effort_timestamp : m_frame->pts;
        const Rational time = ts != AV_NOPTS_VALUE ? timestampToTime(ts, m_timeBase) - m_startTime : Rational();
        m_decodedEnd = timeToFrames(time, Rational(m_outRate), Rounding::Nearest);
    }

    const int maxOut = swr_get_out_samples(m_swr.get(), m_frame->nb_samples);
    std::vector<float> converted(static_cast<std::size_t>(std::max(0, maxOut)) * static_cast<std::size_t>(m_outChannels));
    auto* outPtr = reinterpret_cast<std::uint8_t*>(converted.data());
    const int got = swr_convert(m_swr.get(), &outPtr, maxOut, const_cast<const std::uint8_t**>(m_frame->extended_data),
                                m_frame->nb_samples);
    av_frame_unref(m_frame.get());
    checkFfmpeg(got, "Resampling audio");
    converted.resize(static_cast<std::size_t>(got) * static_cast<std::size_t>(m_outChannels));

    // Align with the seek target: drop samples before it, pad silence if the stream starts later.
    std::int64_t start = m_decodedEnd;
    m_decodedEnd += got;
    std::size_t skip = 0;
    if (start < m_position) {
        const auto drop = std::min<std::int64_t>(m_position - start, got);
        skip = static_cast<std::size_t>(drop) * static_cast<std::size_t>(m_outChannels);
        start += drop;
    }
    const std::int64_t buffered = static_cast<std::int64_t>((m_buffer.size() - m_bufferOffset) / static_cast<std::size_t>(m_outChannels));
    const std::int64_t expected = m_position + buffered;
    if (start > expected) {
        m_buffer.insert(m_buffer.end(), static_cast<std::size_t>(start - expected) * static_cast<std::size_t>(m_outChannels), 0.0f);
    }
    m_buffer.insert(m_buffer.end(), converted.begin() + static_cast<std::ptrdiff_t>(skip), converted.end());
    return true;
}

std::size_t AudioDecoder::read(float* out, std::size_t frames) {
    const auto ch = static_cast<std::size_t>(m_outChannels);
    std::size_t written = 0;
    while (written < frames) {
        const std::size_t availableFrames = (m_buffer.size() - m_bufferOffset) / ch;
        if (availableFrames == 0) {
            m_buffer.clear();
            m_bufferOffset = 0;
            if (m_eof || !decodeMore()) {
                break;
            }
            continue;
        }
        const std::size_t n = std::min(availableFrames, frames - written);
        std::memcpy(out + written * ch, m_buffer.data() + m_bufferOffset, n * ch * sizeof(float));
        m_bufferOffset += n * ch;
        written += n;
        m_position += static_cast<std::int64_t>(n);
    }
    return written;
}

Rational AudioDecoder::position() const {
    return Rational(m_position, m_outRate);
}

} // namespace loki::media
