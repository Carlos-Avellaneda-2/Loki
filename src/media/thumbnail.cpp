#include "media/thumbnail.h"

#include "media/video_decoder.h"
#include "model/project_json.h"

namespace loki::media {

VideoFramePtr makeThumbnail(const std::filesystem::path& path, const Rational& at, int maxWidth, int maxHeight) {
    VideoDecoderOptions options;
    options.allowHardware = false;
    options.rgbaOutput = true;
    options.maxWidth = maxWidth;
    options.maxHeight = maxHeight;
    options.threads = 1;
    VideoDecoder decoder(path, options);
    Rational t = at;
    if (decoder.info().duration.isPositive() && t >= decoder.info().duration) {
        t = decoder.info().duration / Rational(2);
    }
    auto frame = decoder.frameAt(t);
    if (!frame) {
        decoder.seek(Rational());
        frame = decoder.nextFrame();
    }
    if (!frame) {
        throw MediaError("No decodable frame in " + model::pathToUtf8(path));
    }
    return frame;
}

} // namespace loki::media
