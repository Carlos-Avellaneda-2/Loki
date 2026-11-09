#include "export/proxy.h"

#include "export/media_writer.h"
#include "media/video_decoder.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <optional>

namespace loki::exporter {

std::pair<int, int> proxySize(int width, int height) {
    if (width <= 0 || height <= 0) {
        return {kProxyMaxWidth, kProxyMaxHeight};
    }
    const double scale = std::min({1.0, static_cast<double>(kProxyMaxWidth) / width,
                                   static_cast<double>(kProxyMaxHeight) / height});
    auto even = [](double v) { return std::max(2, static_cast<int>(std::lround(v / 2.0)) * 2); };
    return {even(width * scale), even(height * scale)};
}

std::filesystem::path proxyPathFor(const std::filesystem::path& cacheRoot, model::MediaId id) {
    return cacheRoot / "proxies" / (id.toString() + "_720p.mp4");
}

bool canHaveProxy(const model::MediaItem& media) {
    return !media.isGenerated() && media.info.hasVideo && !media.info.isStill && media.info.duration.isPositive();
}

ProxyResult makeProxy(const model::MediaItem& media, const std::filesystem::path& output,
                      const std::atomic<bool>* cancel, const std::function<void(double)>& progress) {
    ProxyResult result;
    auto partial = output;
    partial += ".part";
    try {
        if (!canHaveProxy(media)) {
            throw std::runtime_error("This item does not need a proxy");
        }
        std::filesystem::create_directories(output.parent_path());
        const auto [w, h] = proxySize(media.info.width, media.info.height);
        media::VideoDecoderOptions options;
        options.allowHardware = true;
        options.maxWidth = w; // decode at proxy size when the decoder can
        options.maxHeight = h;
        media::VideoDecoder decoder(media.path, options);
        const Rational rate = media.info.frameRate.isPositive() ? media.info.frameRate : Rational(30);
        VideoSettings video;
        video.codec = VideoCodec::H264;
        video.width = w;
        video.height = h;
        video.frameRate = rate;
        video.bitRate = 4'000'000;
        video.gop = kProxyGop;
        AudioSettings audio;
        audio.codec = AudioCodec::None;
        {
            // .part has no known extension: name the real container through a temporary .mp4.
            auto temp = output;
            temp.replace_extension(".part.mp4");
            partial = temp;
            MediaWriter writer(temp, video, audio);
            const auto frames = timeToFrames(media.info.duration, rate, Rounding::Ceil);
            for (std::int64_t i = 0; i < frames; ++i) {
                if (cancel != nullptr && cancel->load()) {
                    result.cancelled = true;
                    break;
                }
                const auto frame = decoder.frameAt(framesToTime(i, rate));
                if (frame) {
                    writer.writeVideoFrame(*frame);
                }
                if (progress && (i % 10 == 0 || i + 1 == frames)) {
                    progress(static_cast<double>(i + 1) / static_cast<double>(frames));
                }
            }
            if (!result.cancelled) {
                writer.finish();
            }
        }
        if (!result.cancelled) {
            std::filesystem::rename(partial, output);
            result.ok = true;
        }
    } catch (const std::exception& e) {
        result.error = e.what();
        spdlog::warn("Proxy for {} failed: {}", media.name, e.what());
    }
    if (!result.ok) {
        std::error_code ec;
        std::filesystem::remove(partial, ec);
    }
    return result;
}

} // namespace loki::exporter
