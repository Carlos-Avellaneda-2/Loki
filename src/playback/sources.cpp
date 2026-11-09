#include "playback/sources.h"

#include "media/audio_decoder.h"
#include "media/video_decoder.h"
#include "model/effect_registry.h"
#include "model/project_json.h"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace loki::playback {

namespace {

constexpr std::size_t kMaxOpenVideoDecoders = 8;
constexpr int kReverseRun = 8;

std::optional<TimedFrame> timed(const media::VideoFramePtr& f) {
    if (!f) {
        return std::nullopt;
    }
    return TimedFrame{f->pts, f->duration, f};
}

} // namespace

// --- FileVideoSource ---------------------------------------------------------------------------

FileVideoSource::FileVideoSource(const std::filesystem::path& path, bool allowHardware, int maxWidth, int maxHeight) {
    media::VideoDecoderOptions options;
    options.allowHardware = allowHardware;
    options.maxWidth = maxWidth;
    options.maxHeight = maxHeight;
    m_decoder = std::make_unique<media::VideoDecoder>(path, options);
}

FileVideoSource::~FileVideoSource() = default;

std::optional<TimedFrame> FileVideoSource::frameAt(const Rational& t) {
    return timed(m_decoder->frameAt(t));
}

void FileVideoSource::seek(const Rational& t) {
    m_decoder->seek(t);
}

std::optional<TimedFrame> FileVideoSource::nextFrame() {
    return timed(m_decoder->nextFrame());
}

std::vector<TimedFrame> FileVideoSource::framesUpTo(const Rational& t) {
    std::vector<TimedFrame> out;
    for (const auto& f : m_decoder->framesUpTo(t)) {
        out.push_back(TimedFrame{f->pts, f->duration, f});
    }
    return out;
}

void FileVideoSource::setMaxSize(int maxWidth, int maxHeight) {
    m_decoder->setMaxSize(maxWidth, maxHeight);
}

std::string FileVideoSource::hwAccel() const {
    return media::hwAccelName(m_decoder->hwAccel());
}

Rational FileVideoSource::frameRate() const {
    return m_decoder->info().frameRate;
}

// --- FileAudioSource ---------------------------------------------------------------------------

FileAudioSource::FileAudioSource(const std::filesystem::path& path, int sampleRate, int channels)
    : m_decoder(std::make_unique<media::AudioDecoder>(path, sampleRate, channels)) {}

FileAudioSource::~FileAudioSource() = default;

void FileAudioSource::seek(const Rational& t) {
    m_decoder->seek(t);
}

std::size_t FileAudioSource::read(float* out, std::size_t frames) {
    return m_decoder->read(out, frames);
}

// --- SequenceVideoSource -----------------------------------------------------------------------

SequenceVideoSource::SequenceVideoSource(std::shared_ptr<const SequenceSnapshot> snapshot, bool allowHardware,
                                         int maxWidth, int maxHeight)
    : m_snapshot(std::move(snapshot)), m_allowHardware(allowHardware), m_maxWidth(maxWidth), m_maxHeight(maxHeight),
      m_frameDuration(m_snapshot->sequence.settings.frameRate.reciprocal()) {}

SequenceVideoSource::~SequenceVideoSource() = default;

media::VideoDecoder* SequenceVideoSource::decoderFor(const model::Clip& clip, const model::MediaItem& media) {
    if (m_offline.contains(media.id)) {
        return nullptr;
    }
    for (auto it = m_decoders.begin(); it != m_decoders.end(); ++it) {
        if (it->first == clip.id) {
            m_decoders.splice(m_decoders.begin(), m_decoders, it);
            return m_decoders.front().second.get();
        }
    }
    try {
        media::VideoDecoderOptions options;
        options.allowHardware = m_allowHardware && !media.info.isStill;
        options.maxWidth = m_maxWidth;
        options.maxHeight = m_maxHeight;
        m_decoders.emplace_front(clip.id, std::make_unique<media::VideoDecoder>(media.path, options));
    } catch (const std::exception& e) {
        spdlog::warn("Media offline for playback ({}): {}", model::pathToUtf8(media.path), e.what());
        m_offline.insert(media.id);
        return nullptr;
    }
    if (m_decoders.size() > kMaxOpenVideoDecoders) {
        m_decoders.pop_back();
    }
    return m_decoders.front().second.get();
}

std::optional<TimedFrame> SequenceVideoSource::frameAt(const Rational& requested) {
    const auto& seq = m_snapshot->sequence;
    const Rational t = quantizeToFrame(max(Rational(), requested), seq.settings.frameRate, Rounding::Floor);
    if (t >= seq.duration()) {
        return std::nullopt;
    }
    auto composite = std::make_shared<render::CompositeFrame>();
    composite->width = seq.settings.width;
    composite->height = seq.settings.height;
    // V1 (index 0) is the bottom layer.
    for (const auto& track : seq.videoTracks) {
        if (track.state.hidden) {
            continue;
        }
        const auto clip = std::find_if(track.clips.begin(), track.clips.end(),
                                       [&](const model::Clip& c) { return c.start <= t && t < c.end(); });
        if (clip == track.clips.end() || !clip->enabled) {
            continue;
        }
        const auto media = m_snapshot->media.find(clip->media);
        if (media == m_snapshot->media.end() || !media->second.info.hasVideo) {
            continue;
        }
        auto* decoder = decoderFor(*clip, media->second);
        if (decoder == nullptr) {
            continue;
        }
        const Rational sourceTime = clip->toSourceTime(t);
        render::Layer layer;
        layer.frame = decoder->frameAt(sourceTime);
        if (!layer.frame) {
            continue;
        }
        layer.sourceWidth = media->second.info.width > 0 ? media->second.info.width : layer.frame->width;
        layer.sourceHeight = media->second.info.height > 0 ? media->second.info.height : layer.frame->height;
        layer.transform = model::resolveTransform(*clip, sourceTime, media->second.info, seq.settings);
        composite->layers.push_back(std::move(layer));
    }
    TimedFrame result{t, m_frameDuration, nullptr, nullptr};
    if (!composite->layers.empty()) {
        result.frame = composite->layers.back().frame;
        result.composite = std::move(composite);
    }
    return result;
}

void SequenceVideoSource::seek(const Rational& t) {
    m_next = quantizeToFrame(max(Rational(), t), m_snapshot->sequence.settings.frameRate, Rounding::Floor);
}

std::optional<TimedFrame> SequenceVideoSource::nextFrame() {
    auto f = frameAt(m_next);
    m_next += m_frameDuration;
    return f;
}

std::vector<TimedFrame> SequenceVideoSource::framesUpTo(const Rational& t) {
    std::vector<TimedFrame> out;
    const Rational end = quantizeToFrame(max(Rational(), t), m_snapshot->sequence.settings.frameRate, Rounding::Floor);
    Rational start = end - m_frameDuration * Rational(kReverseRun - 1);
    if (start.isNegative()) {
        start = Rational();
    }
    // Ascending order keeps each clip decoder moving forward (one seek per run).
    for (Rational at = start; at <= end; at += m_frameDuration) {
        if (auto f = frameAt(at)) {
            out.push_back(std::move(*f));
        }
    }
    return out;
}

void SequenceVideoSource::setMaxSize(int maxWidth, int maxHeight) {
    m_maxWidth = maxWidth;
    m_maxHeight = maxHeight;
    for (auto& [id, decoder] : m_decoders) {
        decoder->setMaxSize(maxWidth, maxHeight);
    }
}

// --- SequenceAudioSource -----------------------------------------------------------------------

SequenceAudioSource::SequenceAudioSource(std::shared_ptr<const SequenceSnapshot> snapshot, int sampleRate, int channels)
    : m_snapshot(std::move(snapshot)), m_rate(sampleRate), m_channels(channels) {
    m_end = timeToFrames(m_snapshot->sequence.duration(), Rational(m_rate), Rounding::Ceil);
}

SequenceAudioSource::~SequenceAudioSource() = default;

void SequenceAudioSource::seek(const Rational& t) {
    m_position = timeToFrames(max(Rational(), t), Rational(m_rate), Rounding::Nearest);
}

std::size_t SequenceAudioSource::read(float* out, std::size_t frames) {
    if (m_position >= m_end) {
        return 0;
    }
    const auto n = static_cast<std::int64_t>(std::min<std::int64_t>(static_cast<std::int64_t>(frames), m_end - m_position));
    const auto ch = static_cast<std::size_t>(m_channels);
    std::fill_n(out, static_cast<std::size_t>(n) * ch, 0.0f);
    const std::int64_t blockStart = m_position;
    const std::int64_t blockEnd = m_position + n;
    const Rational rate(m_rate);

    for (const auto& track : m_snapshot->sequence.audioTracks) {
        if (track.state.muted) {
            continue;
        }
        for (const auto& clip : track.clips) {
            if (!clip.enabled) {
                continue;
            }
            const auto clipStart = timeToFrames(clip.start, rate, Rounding::Nearest);
            const auto clipEnd = timeToFrames(clip.end(), rate, Rounding::Nearest);
            const auto segStart = std::max(blockStart, clipStart);
            const auto segEnd = std::min(blockEnd, clipEnd);
            if (segStart >= segEnd) {
                continue;
            }
            const auto media = m_snapshot->media.find(clip.media);
            if (media == m_snapshot->media.end() || !media->second.info.hasAudio || m_offline.contains(clip.media)) {
                continue;
            }
            auto& reader = m_readers[clip.id];
            if (!reader.decoder) {
                try {
                    reader.decoder = std::make_unique<media::AudioDecoder>(media->second.path, m_rate, m_channels);
                } catch (const std::exception& e) {
                    spdlog::warn("Audio offline for playback ({}): {}", model::pathToUtf8(media->second.path), e.what());
                    m_offline.insert(clip.media);
                    m_readers.erase(clip.id);
                    continue;
                }
            }
            if (reader.nextFrame != segStart) {
                reader.decoder->seek(clip.toSourceTime(Rational(segStart, m_rate)));
            }
            const auto count = static_cast<std::size_t>(segEnd - segStart);
            m_scratch.assign(count * ch, 0.0f);
            const auto got = reader.decoder->read(m_scratch.data(), count);
            float* dst = out + static_cast<std::size_t>(segStart - blockStart) * ch;
            for (std::size_t i = 0; i < got * ch; ++i) {
                dst[i] += m_scratch[i];
            }
            reader.nextFrame = segEnd;
        }
    }
    for (std::size_t i = 0; i < static_cast<std::size_t>(n) * ch; ++i) {
        out[i] = std::clamp(out[i], -1.0f, 1.0f);
    }
    // Drop readers of clips that are behind the play position.
    std::erase_if(m_readers, [&](const auto& entry) { return entry.second.nextFrame >= 0 && entry.second.nextFrame < blockStart; });
    m_position = blockEnd;
    return static_cast<std::size_t>(n);
}

} // namespace loki::playback
