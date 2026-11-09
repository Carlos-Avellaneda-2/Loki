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

bool sameExceptAudioMix(const SequenceSnapshot& a, const SequenceSnapshot& b) {
    if (a.media != b.media) {
        return false;
    }
    const auto normalized = [](model::Sequence seq) {
        seq.masterVolumeDb = 0.0;
        for (auto& track : seq.audioTracks) {
            track.state.volumeDb = 0.0;
            track.state.pan = 0.0;
            track.state.muted = false;
            track.state.solo = false;
            for (auto& clip : track.clips) {
                clip.fadeIn = {};
                clip.fadeOut = {};
                std::erase_if(clip.effects, [](const model::Effect& e) {
                    return e.type == model::kVolumeEffect || e.type == model::kPannerEffect;
                });
            }
        }
        return seq;
    };
    return normalized(a.sequence) == normalized(b.sequence);
}

SequenceAudioSource::SequenceAudioSource(std::shared_ptr<const SequenceSnapshot> snapshot, int sampleRate, int channels)
    : m_snapshot(std::move(snapshot)), m_rate(sampleRate), m_channels(channels) {
    m_end = timeToFrames(m_snapshot->sequence.duration(), Rational(m_rate), Rounding::Ceil);
}

SequenceAudioSource::~SequenceAudioSource() = default;

void SequenceAudioSource::seek(const Rational& t) {
    m_position = timeToFrames(max(Rational(), t), Rational(m_rate), Rounding::Nearest);
    m_rampsValid = false;
}

void SequenceAudioSource::updateMix(std::shared_ptr<const SequenceSnapshot> snapshot) {
    m_snapshot = std::move(snapshot);
}

namespace {

// Gains are re-evaluated every this many frames inside a clip (keyframes, fades) and ramped
// linearly in between.
constexpr std::int64_t kGainStep = 64;

} // namespace

void SequenceAudioSource::mixClip(const model::Clip& clip, const model::MediaItem& media, std::int64_t segStart,
                                  std::int64_t segEnd, std::int64_t blockStart, float* bus) {
    auto& reader = m_readers[clip.id];
    if (!reader.decoder) {
        try {
            reader.decoder = std::make_unique<media::AudioDecoder>(media.path, m_rate, m_channels);
        } catch (const std::exception& e) {
            spdlog::warn("Audio offline for playback ({}): {}", model::pathToUtf8(media.path), e.what());
            m_offline.insert(clip.media);
            m_readers.erase(clip.id);
            return;
        }
    }
    if (reader.nextFrame != segStart) {
        reader.decoder->seek(clip.toSourceTime(Rational(segStart, m_rate)));
    }
    const auto ch = static_cast<std::size_t>(m_channels);
    const auto count = static_cast<std::size_t>(segEnd - segStart);
    m_scratch.assign(count * ch, 0.0f);
    const auto got = static_cast<std::int64_t>(reader.decoder->read(m_scratch.data(), count));
    reader.nextFrame = segEnd;

    const bool mono = media.info.audioChannels == 1;
    const double duration = clip.duration.toDouble();
    const double fadeIn = clip.fadeIn.toDouble();
    const double fadeOut = clip.fadeOut.toDouble();
    const auto gainAt = [&](std::int64_t frame) {
        const Rational t(frame, m_rate);
        const auto mix = model::resolveAudioMix(clip, clip.toSourceTime(t));
        const auto fade = audio::clipFadeGain((t - clip.start).toDouble(), duration, fadeIn, fadeOut);
        const auto pan = mono ? audio::monoPanGains(mix.pan) : audio::balanceGains(mix.pan);
        return pan * static_cast<float>(audio::dbToGain(mix.levelDb) * fade);
    };
    audio::StereoGains from = gainAt(segStart);
    for (std::int64_t f = segStart; f < segStart + got; f += kGainStep) {
        const auto stepEnd = std::min(segStart + got, f + kGainStep);
        const auto to = gainAt(stepEnd);
        const auto offset = static_cast<std::size_t>(f - segStart) * ch;
        audio::mixRamp(bus + static_cast<std::size_t>(f - blockStart) * ch, m_scratch.data() + offset,
                       static_cast<std::size_t>(stepEnd - f), m_channels, from, to);
        from = to;
    }
}

std::size_t SequenceAudioSource::read(float* out, std::size_t frames) {
    m_levels.clear();
    if (m_position >= m_end) {
        return 0;
    }
    const auto n = static_cast<std::int64_t>(std::min<std::int64_t>(static_cast<std::int64_t>(frames), m_end - m_position));
    const auto nFrames = static_cast<std::size_t>(n);
    const auto ch = static_cast<std::size_t>(m_channels);
    std::fill_n(out, nFrames * ch, 0.0f);
    const std::int64_t blockStart = m_position;
    const std::int64_t blockEnd = m_position + n;
    const Rational rate(m_rate);
    const auto& seq = m_snapshot->sequence;
    const bool anySolo = std::any_of(seq.audioTracks.begin(), seq.audioTracks.end(),
                                     [](const model::Track& t) { return t.state.solo; });

    for (const auto& track : seq.audioTracks) {
        const bool audible = !track.state.muted && (!anySolo || track.state.solo);
        const auto target = audible ? audio::balanceGains(track.state.pan) * audio::dbToGain(track.state.volumeDb)
                                    : audio::StereoGains{0.0f, 0.0f};
        auto [prev, inserted] = m_trackGains.try_emplace(track.id, target);
        const auto from = (inserted || !m_rampsValid) ? target : prev->second;
        prev->second = target;
        if (!audible && from == target) {
            m_levels.push_back({track.id, {}});
            continue; // silent for the whole block: skip decoding (readers re-seek when heard again)
        }
        m_bus.assign(nFrames * ch, 0.0f);
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
            mixClip(clip, media->second, segStart, segEnd, blockStart, m_bus.data());
        }
        audio::applyRamp(m_bus.data(), nFrames, m_channels, from, target);
        m_levels.push_back({track.id, audio::measure(m_bus.data(), nFrames, m_channels)});
        for (std::size_t i = 0; i < nFrames * ch; ++i) {
            out[i] += m_bus[i];
        }
    }
    const auto master = audio::StereoGains{1.0f, 1.0f} * audio::dbToGain(seq.masterVolumeDb);
    audio::applyRamp(out, nFrames, m_channels, m_rampsValid ? m_masterGain : master, master);
    m_masterGain = master;
    m_rampsValid = true;
    audio::clampSamples(out, nFrames * ch);
    // Drop readers of clips that are behind the play position.
    std::erase_if(m_readers, [&](const auto& entry) { return entry.second.nextFrame >= 0 && entry.second.nextFrame < blockStart; });
    m_position = blockEnd;
    return nFrames;
}

} // namespace loki::playback
