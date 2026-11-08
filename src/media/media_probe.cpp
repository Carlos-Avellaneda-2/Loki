#include "media/media_probe.h"

#include "media/ffmpeg_utils.h"
#include "model/project_json.h"

#include <algorithm>

namespace loki::media {

namespace {

bool isAttachedPicture(const AVStream* s) {
    return (s->disposition & AV_DISPOSITION_ATTACHED_PIC) != 0;
}

std::string codecName(const AVCodecParameters* p) {
    const auto* desc = avcodec_descriptor_get(p->codec_id);
    return desc != nullptr ? desc->name : "unknown";
}

bool isImageContainer(const AVFormatContext* ctx) {
    const std::string name = ctx->iformat != nullptr ? ctx->iformat->name : "";
    return name.find("_pipe") != std::string::npos || name == "image2" || name == "png_pipe";
}

} // namespace

model::MediaInfo probeMedia(const std::filesystem::path& path) {
    const auto ctx = openInput(path);
    model::MediaInfo info;

    const int videoIndex = av_find_best_stream(ctx.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    const int audioIndex = av_find_best_stream(ctx.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);

    if (videoIndex >= 0 && !isAttachedPicture(ctx->streams[videoIndex])) {
        const auto* stream = ctx->streams[videoIndex];
        const auto* par = stream->codecpar;
        info.hasVideo = true;
        info.width = par->width;
        info.height = par->height;
        info.videoCodec = codecName(par);
        info.frameRate = streamFrameRate(stream);
        info.isStill = isImageContainer(ctx.get()) || stream->nb_frames == 1;
        if (stream->duration != AV_NOPTS_VALUE && stream->duration > 0) {
            info.duration = timestampToTime(stream->duration, stream->time_base);
        }
    }

    if (audioIndex >= 0) {
        const auto* stream = ctx->streams[audioIndex];
        const auto* par = stream->codecpar;
        info.hasAudio = true;
        info.sampleRate = par->sample_rate;
        info.audioChannels = par->ch_layout.nb_channels;
        info.audioCodec = codecName(par);
        if (stream->duration != AV_NOPTS_VALUE && stream->duration > 0) {
            info.duration = max(info.duration, timestampToTime(stream->duration, stream->time_base));
        }
    }

    if (!info.hasVideo && !info.hasAudio) {
        throw MediaError("No audio or video stream in " + model::pathToUtf8(path));
    }
    if (info.duration.isZero() && ctx->duration > 0 && !info.isStill) {
        info.duration = Rational(ctx->duration, AV_TIME_BASE);
    }
    if (info.isStill) {
        info.duration = Rational();
        info.frameRate = Rational();
    }
    return info;
}

const std::vector<std::string>& importableExtensions() {
    static const std::vector<std::string> kExtensions = {
        ".mp4", ".mov", ".m4v", ".mkv", ".avi", ".mxf", ".mts", ".m2ts", ".ts", ".webm", ".wmv", ".mpg", ".mpeg",
        ".wav", ".mp3", ".aac", ".m4a", ".flac", ".ogg", ".opus", ".aif", ".aiff",
        ".png", ".jpg", ".jpeg", ".bmp", ".tif", ".tiff",
    };
    return kExtensions;
}

} // namespace loki::media
