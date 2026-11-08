#include "media/ffmpeg_utils.h"

#include "model/project_json.h"

#include <spdlog/spdlog.h>

#include <array>
#include <cstdarg>
#include <cstdio>
#include <limits>

namespace loki::media {

FramePtr makeFrame() {
    FramePtr frame(av_frame_alloc());
    if (!frame) {
        throw MediaError("av_frame_alloc failed");
    }
    return frame;
}

PacketPtr makePacket() {
    PacketPtr packet(av_packet_alloc());
    if (!packet) {
        throw MediaError("av_packet_alloc failed");
    }
    return packet;
}

std::string ffmpegErrorString(int error) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
    av_strerror(error, buffer.data(), buffer.size());
    return buffer.data();
}

int checkFfmpeg(int error, const std::string& what) {
    if (error < 0) {
        throw MediaError(what + ": " + ffmpegErrorString(error));
    }
    return error;
}

std::string pathToFfmpeg(const std::filesystem::path& path) {
    return model::pathToUtf8(path); // FFmpeg's file protocol expects UTF-8 on Windows
}

FormatContextPtr openInput(const std::filesystem::path& path) {
    AVFormatContext* raw = nullptr;
    checkFfmpeg(avformat_open_input(&raw, pathToFfmpeg(path).c_str(), nullptr, nullptr),
                "Cannot open " + model::pathToUtf8(path));
    FormatContextPtr ctx(raw);
    checkFfmpeg(avformat_find_stream_info(ctx.get(), nullptr), "Cannot read stream info of " + model::pathToUtf8(path));
    return ctx;
}

Rational toRational(AVRational r) {
    if (r.den == 0) {
        return {};
    }
    return Rational(r.num, r.den);
}

AVRational toAVRational(const Rational& r) {
    constexpr auto kMax = std::numeric_limits<int>::max();
    if (r.num() > kMax || r.num() < -kMax || r.den() > kMax) {
        throw MediaError("Rational " + r.toString() + " does not fit AVRational");
    }
    return AVRational{static_cast<int>(r.num()), static_cast<int>(r.den())};
}

Rational timestampToTime(std::int64_t ts, AVRational timeBase) {
    return Rational(ts) * toRational(timeBase);
}

std::int64_t timeToTimestamp(const Rational& time, AVRational timeBase) {
    return (time / toRational(timeBase)).floor();
}

Rational streamFrameRate(const AVStream* stream) {
    const AVRational r = stream->r_frame_rate;
    const AVRational avg = stream->avg_frame_rate;
    const bool rValid = r.num > 0 && r.den > 0;
    const bool avgValid = avg.num > 0 && avg.den > 0;
    AVRational chosen = rValid ? r : avg;
    if (rValid && avgValid) {
        const double rv = av_q2d(r);
        const double av = av_q2d(avg);
        if (rv > av * 1.9 && rv < av * 2.1) {
            chosen = avg; // r is the field rate of interlaced material
        }
    }
    if (chosen.num <= 0 || chosen.den <= 0) {
        return {};
    }
    static const Rational kStandard[] = {Rational(24000, 1001), Rational(24), Rational(25), Rational(30000, 1001),
                                         Rational(30),          Rational(48), Rational(50), Rational(60000, 1001),
                                         Rational(60),          Rational(120)};
    const Rational rate = toRational(chosen);
    for (const auto& s : kStandard) {
        const double ratio = (rate / s).toDouble();
        if (ratio > 0.999 && ratio < 1.001) {
            return s;
        }
    }
    return rate;
}

namespace {

void ffmpegLogCallback(void* avcl, int level, const char* fmt, va_list args) {
    if (level > av_log_get_level()) {
        return;
    }
    std::array<char, 1024> line{};
    int printPrefix = 1;
    av_log_format_line2(avcl, level, fmt, args, line.data(), static_cast<int>(line.size()), &printPrefix);
    std::string text(line.data());
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
    }
    if (text.empty()) {
        return;
    }
    if (level <= AV_LOG_ERROR) {
        spdlog::error("[ffmpeg] {}", text);
    } else if (level <= AV_LOG_WARNING) {
        spdlog::warn("[ffmpeg] {}", text);
    } else {
        spdlog::debug("[ffmpeg] {}", text);
    }
}

} // namespace

void installFfmpegLogBridge() {
    av_log_set_level(AV_LOG_WARNING);
    av_log_set_callback(&ffmpegLogCallback);
}

} // namespace loki::media
