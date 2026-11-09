#include "playback/sources.h"

#include "media/audio_decoder.h"
#include "media/video_decoder.h"
#include "model/effect_registry.h"
#include "model/project_json.h"
#include "model/transitions.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <future>

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
    return timed(m_decoder->frameAt(t, true)); // seeks/scrubs (playback uses nextFrame)
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

std::filesystem::path videoPathFor(const model::MediaItem& media, bool useProxies) {
    if (useProxies && media.proxyPath) {
        std::error_code ec;
        if (std::filesystem::exists(*media.proxyPath, ec)) {
            return *media.proxyPath;
        }
    }
    return media.path;
}

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
        m_decoders.emplace_front(clip.id,
                                 std::make_unique<media::VideoDecoder>(videoPathFor(media, m_snapshot->useProxies), options));
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

std::shared_ptr<const render::Lut3D> SequenceVideoSource::lutFor(const std::string& path) {
    if (const auto it = m_luts.find(path); it != m_luts.end()) {
        return it->second;
    }
    std::shared_ptr<const render::Lut3D> lut;
    try {
        lut = render::loadCubeLut(path);
    } catch (const std::exception& e) {
        spdlog::warn("LUT not applied: {}", e.what());
    }
    m_luts.emplace(path, lut);
    return lut;
}

media::VideoFramePtr SequenceVideoSource::titleFrame(const model::Clip& clip, const Rational& sourceTime) {
    const auto& settings = m_snapshot->sequence.settings;
    static const model::MediaInfo kCanvas;
    const auto value = [&](const char* key) {
        return model::effectiveValue(clip, model::kTextEffect, key, sourceTime, kCanvas, settings);
    };
    text::TitleSpec spec;
    spec.text = model::effectiveString(clip, model::kTextEffect, "text");
    spec.font = model::effectiveString(clip, model::kTextEffect, "font");
    spec.color = model::effectiveString(clip, model::kTextEffect, "color");
    spec.size = value("size");
    spec.x = value("positionX");
    spec.y = value("positionY");
    spec.shadow = value("shadow") >= 0.5;
    spec.shadowDistance = value("shadowDistance");
    spec.shadowOpacity = value("shadowOpacity");
    spec.canvasWidth = settings.width;
    spec.canvasHeight = settings.height;
    auto& cached = m_titles[clip.id];
    if (!cached.frame || !(cached.spec == spec)) {
        cached.spec = spec;
        cached.frame = text::renderTitle(spec);
    }
    return cached.frame;
}

Rational SequenceVideoSource::sourceTimeFor(const model::Clip& clip, const model::MediaItem& media,
                                            const Rational& t) const {
    Rational sourceTime = max(Rational(), clip.toSourceTime(t));
    if (!media.isGenerated() && !media.info.isStill && media.info.duration.isPositive() &&
        sourceTime >= media.info.duration) {
        sourceTime = max(Rational(), media.info.duration - m_frameDuration); // freeze the last frame
    }
    return sourceTime;
}

void SequenceVideoSource::prefetch(const Rational& t) {
    std::vector<std::pair<media::VideoDecoder*, Rational>> work;
    const auto want = [&](const model::Clip* clip) {
        if (clip == nullptr || !clip->enabled) {
            return;
        }
        const auto media = m_snapshot->media.find(clip->media);
        if (media == m_snapshot->media.end() || !media->second.info.hasVideo || media->second.isGenerated()) {
            return;
        }
        if (auto* decoder = decoderFor(*clip, media->second)) {
            work.emplace_back(decoder, sourceTimeFor(*clip, media->second, t));
        }
    };
    for (const auto& track : m_snapshot->sequence.videoTracks) {
        if (track.state.hidden) {
            continue;
        }
        if (const auto active = model::activeTransition(track, t)) {
            want(active->span.from);
            want(active->span.to);
            continue;
        }
        const auto clip = std::find_if(track.clips.begin(), track.clips.end(),
                                       [&](const model::Clip& c) { return c.start <= t && t < c.end(); });
        if (clip != track.clips.end()) {
            want(&*clip);
        }
    }
    if (work.size() == 1) {
        (void)work.front().first->frameAt(work.front().second, true); // single layer: scrub jump only
        return;
    }
    if (work.empty() || work.size() > kMaxOpenVideoDecoders) {
        return; // nothing to do (or the decoder LRU could evict one we hold)
    }
    // Each decoder belongs to one clip, so they can run concurrently; the first stays on this thread.
    std::vector<std::future<void>> others;
    for (std::size_t i = 1; i < work.size(); ++i) {
        others.push_back(std::async(std::launch::async, [job = work[i]] { (void)job.first->frameAt(job.second, true); }));
    }
    (void)work.front().first->frameAt(work.front().second, true);
    for (auto& f : others) {
        f.get();
    }
}

std::optional<render::Layer> SequenceVideoSource::layerFor(const model::Clip& clip, const Rational& t) {
    if (!clip.enabled) {
        return std::nullopt;
    }
    const auto mediaIt = m_snapshot->media.find(clip.media);
    if (mediaIt == m_snapshot->media.end() || !mediaIt->second.info.hasVideo) {
        return std::nullopt;
    }
    const auto& media = mediaIt->second;
    const auto& seq = m_snapshot->sequence;
    const Rational sourceTime = sourceTimeFor(clip, media, t);
    render::Layer layer;
    if (media.generator == model::kTitleGenerator) {
        layer.frame = titleFrame(clip, sourceTime);
        layer.sourceWidth = seq.settings.width;
        layer.sourceHeight = seq.settings.height;
    } else {
        auto* decoder = decoderFor(clip, media);
        if (decoder == nullptr) {
            return std::nullopt;
        }
        layer.frame = decoder->frameAt(sourceTime);
        if (layer.frame) {
            layer.sourceWidth = media.info.width > 0 ? media.info.width : layer.frame->width;
            layer.sourceHeight = media.info.height > 0 ? media.info.height : layer.frame->height;
        }
    }
    if (!layer.frame) {
        return std::nullopt;
    }
    layer.transform = model::resolveTransform(clip, sourceTime, media.info, seq.settings);
    if (const auto* color = model::findEffectByType(clip, model::kColorEffect); color != nullptr && color->enabled) {
        const auto v = [&](const char* key) {
            return static_cast<float>(model::effectiveValue(clip, model::kColorEffect, key, sourceTime, media.info, seq.settings));
        };
        layer.grade.color = {v("exposure"), v("contrast"), v("saturation"), v("temperature")};
    }
    if (const auto* lut = model::findEffectByType(clip, model::kLutEffect); lut != nullptr && lut->enabled) {
        const auto file = lut->string("file");
        if (!file.empty()) {
            layer.grade.lut = lutFor(file);
            layer.grade.lutMix = static_cast<float>(
                model::effectiveValue(clip, model::kLutEffect, "intensity", sourceTime, media.info, seq.settings) / 100.0);
        }
    }
    return layer;
}

std::optional<TimedFrame> SequenceVideoSource::frameAt(const Rational& requested) {
    // Seeks/scrubs: decode the layers in parallel. Sequential playback (nextFrame) does not, so it
    // never competes with the audio thread for the CPU.
    return compose(requested, true);
}

std::optional<TimedFrame> SequenceVideoSource::compose(const Rational& requested, bool parallel) {
    const auto& seq = m_snapshot->sequence;
    const Rational t = quantizeToFrame(max(Rational(), requested), seq.settings.frameRate, Rounding::Floor);
    if (t >= seq.duration()) {
        return std::nullopt;
    }
    if (parallel) {
        prefetch(t);
    }
    auto composite = std::make_shared<render::CompositeFrame>();
    composite->width = seq.settings.width;
    composite->height = seq.settings.height;
    const auto push = [&](std::optional<render::Layer> layer) {
        if (layer) {
            composite->layers.push_back(std::move(*layer));
        }
    };
    // V1 (index 0) is the bottom layer.
    for (const auto& track : seq.videoTracks) {
        if (track.state.hidden) {
            continue;
        }
        if (const auto active = model::activeTransition(track, t)) {
            const auto& span = active->span;
            const auto p = static_cast<float>(span.progress(t));
            auto a = span.from != nullptr ? layerFor(*span.from, t) : std::nullopt;
            auto b = span.to != nullptr ? layerFor(*span.to, t) : std::nullopt;
            const auto& type = active->transition->type;
            if (type == model::kDipToBlack) {
                if (a && b) {
                    if (p < 0.5f) {
                        a->grade.dim = 2.0f * p;
                        b.reset();
                    } else {
                        b->grade.dim = 2.0f * (1.0f - p);
                        a.reset();
                    }
                } else if (a) {
                    a->grade.dim = p;
                } else if (b) {
                    b->grade.dim = 1.0f - p;
                }
            } else if (type == model::kWipe) {
                if (b) {
                    b->wipe = {true, p, 0.02f, false};
                } else if (a) {
                    a->wipe = {true, p, 0.02f, true};
                }
            } else { // Cross Dissolve (and unknown types)
                if (b) {
                    b->transform.opacity *= p;
                } else if (a) {
                    a->transform.opacity *= 1.0f - p;
                }
            }
            push(std::move(a));
            push(std::move(b));
            continue;
        }
        const auto clip = std::find_if(track.clips.begin(), track.clips.end(),
                                       [&](const model::Clip& c) { return c.start <= t && t < c.end(); });
        if (clip != track.clips.end()) {
            push(layerFor(*clip, t));
        }
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
    auto f = compose(m_next, false);
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
        if (auto f = compose(at, false)) {
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

std::shared_ptr<SequenceSnapshot> makeSnapshot(const model::Project& project, model::SequenceId id,
                                               std::uint64_t generation) {
    const auto* seq = project.findSequence(id);
    if (seq == nullptr) {
        return nullptr;
    }
    auto snapshot = std::make_shared<SequenceSnapshot>();
    snapshot->sequence = *seq;
    for (const auto* list : {&seq->videoTracks, &seq->audioTracks}) {
        for (const auto& track : *list) {
            for (const auto& clip : track.clips) {
                if (!snapshot->media.contains(clip.media)) {
                    if (const auto* media = project.findMedia(clip.media)) {
                        snapshot->media.emplace(clip.media, *media);
                    }
                }
            }
        }
    }
    snapshot->generation = generation;
    return snapshot;
}

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
            track.transitions.clear(); // crossfades only change gains
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
    return a.useProxies == b.useProxies && normalized(a.sequence) == normalized(b.sequence);
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
                                  std::int64_t segEnd, std::int64_t blockStart, float* bus,
                                  const std::vector<model::TransitionSpan>& spans) {
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
        auto fade = audio::clipFadeGain((t - clip.start).toDouble(), duration, fadeIn, fadeOut);
        // Constant-power crossfades (D19): the outgoing clip follows cos, the incoming one sin.
        for (const auto& span : spans) {
            if (span.contains(t)) {
                const double p = span.progress(t);
                if (span.from == &clip) {
                    fade *= audio::fadeGain(1.0 - p);
                } else if (span.to == &clip) {
                    fade *= audio::fadeGain(p);
                }
            }
        }
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
        std::vector<model::TransitionSpan> spans;
        for (const auto& transition : track.transitions) {
            if (auto span = model::transitionSpan(track, transition)) {
                spans.push_back(*span);
            }
        }
        for (const auto& clip : track.clips) {
            if (!clip.enabled) {
                continue;
            }
            auto clipStart = timeToFrames(clip.start, rate, Rounding::Nearest);
            auto clipEnd = timeToFrames(clip.end(), rate, Rounding::Nearest);
            // Two-sided crossfades read past the cut (handles).
            for (const auto& span : spans) {
                if (span.from == &clip && span.to != nullptr) {
                    clipEnd = std::max(clipEnd, timeToFrames(span.end, rate, Rounding::Nearest));
                }
                if (span.to == &clip && span.from != nullptr) {
                    const auto earliest = timeToFrames(clip.start - clip.sourceIn, rate, Rounding::Ceil);
                    clipStart = std::max(earliest, std::min(clipStart, timeToFrames(span.start, rate, Rounding::Nearest)));
                }
            }
            const auto segStart = std::max(blockStart, clipStart);
            const auto segEnd = std::min(blockEnd, clipEnd);
            if (segStart >= segEnd) {
                continue;
            }
            const auto media = m_snapshot->media.find(clip.media);
            if (media == m_snapshot->media.end() || !media->second.info.hasAudio || m_offline.contains(clip.media)) {
                continue;
            }
            mixClip(clip, media->second, segStart, segEnd, blockStart, m_bus.data(), spans);
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
