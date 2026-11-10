#include "playback/sources.h"

#include "media/audio_decoder.h"
#include "media/video_decoder.h"
#include "model/effect_registry.h"
#include "model/project_json.h"
#include "model/transitions.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <future>
#include <stdexcept>

namespace loki::playback {

namespace {

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
                                         int maxWidth, int maxHeight, std::size_t maxDecoders, bool parallelPlayback)
    : m_snapshot(std::move(snapshot)), m_allowHardware(allowHardware), m_maxWidth(maxWidth), m_maxHeight(maxHeight),
      m_frameDuration(m_snapshot->sequence.settings.frameRate.reciprocal()),
      m_maxDecoders(std::max<std::size_t>(1, maxDecoders)), m_parallelPlayback(parallelPlayback) {}

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
    if (m_decoders.size() > m_maxDecoders) {
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
    // A reversed clip shows, for the frame [t, t + frame), the source picture at its far end.
    Rational sourceTime = max(Rational(), clip.reverse ? max(clip.sourceIn, clip.toSourceTime(t + m_frameDuration))
                                                       : clip.toSourceTime(t));
    if (!media.isGenerated() && !media.info.isStill && media.info.duration.isPositive() &&
        sourceTime >= media.info.duration) {
        sourceTime = max(Rational(), media.info.duration - m_frameDuration); // freeze the last frame
    }
    return sourceTime;
}

void SequenceVideoSource::prefetch(const Rational& t) {
    std::vector<std::pair<media::VideoDecoder*, Rational>> work;
    const auto want = [&](const model::Clip* clip) {
        if (clip == nullptr || !clip->enabled || clip->reverse) { // reversed clips decode runs (decodedFrame)
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
    if (work.empty() || work.size() > m_maxDecoders) {
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

SequenceVideoSource* SequenceVideoSource::nestedSourceFor(const model::Clip& clip, const model::MediaItem& media) {
    for (auto it = m_nested.begin(); it != m_nested.end(); ++it) {
        if (it->first == clip.id) {
            m_nested.splice(m_nested.begin(), m_nested, it);
            return m_nested.front().second.get();
        }
    }
    const auto nested = m_snapshot->nested.find(media.sequence);
    if (nested == m_snapshot->nested.end() || !nested->second) {
        return nullptr;
    }
    auto snapshot = std::make_shared<SequenceSnapshot>(*nested->second);
    snapshot->useProxies = m_snapshot->useProxies;
    auto source = std::make_unique<SequenceVideoSource>(std::move(snapshot), m_allowHardware, m_maxWidth, m_maxHeight,
                                                        m_maxDecoders, false);
    source->setTransparent(true);
    m_nested.emplace_front(clip.id, std::move(source));
    constexpr std::size_t kMaxNestedSources = 4;
    if (m_nested.size() > kMaxNestedSources) {
        m_nested.pop_back();
    }
    return m_nested.front().second.get();
}

media::VideoFramePtr SequenceVideoSource::decodedFrame(const model::Clip& clip, const model::MediaItem& media,
                                                       const Rational& sourceTime) {
    auto* decoder = decoderFor(clip, media);
    if (decoder == nullptr) {
        return nullptr;
    }
    if (!clip.reverse || media.info.isStill) {
        return decoder->frameAt(sourceTime);
    }
    // Backwards: keep the run decoded from the previous keyframe and serve earlier frames from it.
    const auto find = [&](const std::vector<media::VideoFramePtr>& run) -> media::VideoFramePtr {
        for (auto it = run.rbegin(); it != run.rend(); ++it) {
            if ((*it)->pts <= sourceTime) {
                return sourceTime < (*it)->pts + (*it)->duration ? *it : nullptr;
            }
        }
        return nullptr;
    };
    auto& run = m_reverseRuns[clip.id];
    if (auto f = find(run)) {
        return f;
    }
    run = decoder->framesUpTo(sourceTime);
    constexpr std::size_t kKeptFrames = 24; // bounds memory with long GOPs
    if (run.size() > kKeptFrames) {
        run.erase(run.begin(), run.end() - static_cast<std::ptrdiff_t>(kKeptFrames));
    }
    return run.empty() ? nullptr : run.back();
}

render::CompositePtr SequenceVideoSource::compositeAt(const Rational& t) {
    auto f = compose(t, false);
    return f ? f->composite : nullptr;
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
    } else if (media.isAdjustment()) {
        layer.adjustment = true;
    } else if (media.isNest()) {
        auto* nested = nestedSourceFor(clip, media);
        if (nested != nullptr) {
            nested->setSoloTrack(media.multicam ? clip.angle : -1); // a multicam clip shows one angle
        }
        layer.nested = nested != nullptr ? nested->compositeAt(sourceTime) : nullptr;
        if (!layer.nested) {
            return std::nullopt;
        }
        layer.sourceWidth = layer.nested->width;
        layer.sourceHeight = layer.nested->height;
    } else {
        layer.frame = decodedFrame(clip, media, sourceTime);
        if (layer.frame) {
            layer.sourceWidth = media.info.width > 0 ? media.info.width : layer.frame->width;
            layer.sourceHeight = media.info.height > 0 ? media.info.height : layer.frame->height;
        }
    }
    if (!layer.hasPicture()) {
        return std::nullopt;
    }
    layer.transform = model::resolveTransform(clip, sourceTime, media.info, seq.settings);
    if (layer.adjustment) {
        render::coverCanvas(layer, seq.settings.width, seq.settings.height); // D31: Motion is not applied
    }
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
    // Phase 14: Lumetri, key, blur/sharpen, masks, stabilizer.
    m_effects.apply(clip, media.info, seq.settings, sourceTime, layer);
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
    if (m_snapshot->multicamView) {
        if (auto grid = multicamGrid(t)) {
            return grid;
        }
    }
    if (auto rendered = previewFrame(t)) {
        return rendered;
    }
    if (parallel) {
        prefetch(t);
    }
    auto composite = std::make_shared<render::CompositeFrame>();
    composite->width = seq.settings.width;
    composite->height = seq.settings.height;
    composite->transparent = m_transparent;
    const auto push = [&](std::optional<render::Layer> layer) {
        if (layer) {
            composite->layers.push_back(std::move(*layer));
        }
    };
    // V1 (index 0) is the bottom layer.
    for (std::size_t trackIndex = 0; trackIndex < seq.videoTracks.size(); ++trackIndex) {
        const auto& track = seq.videoTracks[trackIndex];
        if (track.state.hidden || (m_soloTrack >= 0 && static_cast<int>(trackIndex) != m_soloTrack)) {
            continue;
        }
        if (const auto active = model::activeTransition(track, t)) {
            const auto& span = active->span;
            const auto p = static_cast<float>(span.progress(t));
            auto a = span.from != nullptr ? layerFor(*span.from, t) : std::nullopt;
            auto b = span.to != nullptr ? layerFor(*span.to, t) : std::nullopt;
            const auto& type = active->transition->type;
            const double canvasWidth = seq.settings.width;
            const float aspect = static_cast<float>(seq.settings.width) / static_cast<float>(std::max(1, seq.settings.height));
            if (type == model::kDipToWhite) {
                if (a) {
                    a->grade.dimTarget = 1.0f;
                }
                if (b) {
                    b->grade.dimTarget = 1.0f;
                }
            }
            if (type == model::kDipToBlack || type == model::kDipToWhite) {
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
            } else if (type == model::kWipe || type == model::kIrisRound || type == model::kIrisBox) {
                const auto shape = type == model::kIrisRound ? render::WipeShape::Circle
                                   : type == model::kIrisBox ? render::WipeShape::Box
                                                             : render::WipeShape::Linear;
                if (b) {
                    b->wipe = {true, p, 0.02f, false, shape, aspect};
                } else if (a) {
                    a->wipe = {true, p, 0.02f, true, shape, aspect};
                }
            } else if (type == model::kPush || type == model::kSlide) {
                // The incoming clip enters from the right; Push also moves the outgoing one out.
                if (b) {
                    b->transform.positionX += (1.0 - p) * canvasWidth;
                }
                if (a && type == model::kPush) {
                    a->transform.positionX -= p * canvasWidth;
                }
                if (!b && a && type == model::kSlide) {
                    a->transform.positionX -= p * canvasWidth;
                }
            } else if (type == model::kCrossZoom) {
                if (a) {
                    a->transform.scale *= 1.0 + 3.0 * p;
                    a->transform.opacity *= p < 0.5f ? 1.0 : 2.0 * (1.0 - p);
                }
                if (b) {
                    b->transform.scale *= 1.0 + 3.0 * (1.0 - p);
                    b->transform.opacity *= p < 0.5f ? 2.0 * p : 1.0;
                }
            } else if (type == model::kAdditiveDissolve) {
                if (a) {
                    a->transform.opacity *= 1.0f - p;
                }
                if (b) {
                    b->transform.opacity *= p;
                    if (a) {
                        b->transform.blend = model::BlendMode::Add;
                    }
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
    for (auto it = composite->layers.rbegin(); it != composite->layers.rend() && !result.frame; ++it) {
        result.frame = it->frame; // topmost decoded picture (may be none: nests, adjustments)
    }
    push(captionLayer(t));
    if (!composite->layers.empty()) {
        result.composite = std::move(composite);
    }
    return result;
}

// --- Phase 15 -------------------------------------------------------------------------------------

std::optional<render::Layer> SequenceVideoSource::captionLayer(const Rational& t) {
    const auto& seq = m_snapshot->sequence;
    if (!m_snapshot->showCaptions || seq.captions.empty()) {
        return std::nullopt;
    }
    const model::Caption* caption = nullptr;
    for (const auto& c : seq.captions) {
        if (c.start <= t && t < c.end()) {
            caption = &c;
            break;
        }
    }
    if (caption == nullptr || caption->text.empty()) {
        return std::nullopt;
    }
    const auto& style = seq.captionStyle;
    text::TitleSpec spec;
    spec.text = caption->text;
    spec.font = style.font;
    spec.color = style.color;
    spec.size = style.size;
    spec.x = seq.settings.width / 2.0;
    spec.y = seq.settings.height * style.positionY / 100.0;
    spec.shadow = style.backgroundOpacity <= 0.0;
    spec.backgroundOpacity = style.backgroundOpacity;
    spec.canvasWidth = seq.settings.width;
    spec.canvasHeight = seq.settings.height;
    const std::string key = caption->text + '\x1f' + style.font + style.color + std::to_string(style.size) +
                            std::to_string(style.positionY) + std::to_string(style.backgroundOpacity);
    if (key != m_captionText || !m_captionFrame) {
        m_captionText = key;
        m_captionFrame = text::renderTitle(spec);
    }
    if (!m_captionFrame) {
        return std::nullopt;
    }
    render::Layer layer;
    layer.frame = m_captionFrame;
    render::coverCanvas(layer, seq.settings.width, seq.settings.height);
    layer.transform.opacity = 100.0;
    return layer;
}

std::optional<TimedFrame> SequenceVideoSource::previewFrame(const Rational& t) {
    const auto& previews = m_snapshot->previews;
    if (!previews.length.isPositive() || previews.files.empty()) {
        return std::nullopt;
    }
    const auto index = static_cast<std::size_t>(std::max<std::int64_t>(0, (t / previews.length).floor()));
    if (index >= previews.files.size() || previews.files[index].empty()) {
        return std::nullopt;
    }
    auto& decoder = m_previewDecoders[index];
    if (!decoder) {
        try {
            media::VideoDecoderOptions options;
            options.allowHardware = false;
            options.maxWidth = m_maxWidth;
            options.maxHeight = m_maxHeight;
            decoder = std::make_unique<media::VideoDecoder>(previews.files[index], options);
        } catch (const std::exception& e) {
            spdlog::warn("Render preview unreadable ({}): {}", model::pathToUtf8(previews.files[index]), e.what());
            m_previewDecoders.erase(index);
            return std::nullopt;
        }
        // Few open previews at once: playback moves forward segment by segment.
        for (auto it = m_previewDecoders.begin(); it != m_previewDecoders.end();) {
            it = (it->first + 1 < index || it->first > index + 1) ? m_previewDecoders.erase(it) : std::next(it);
        }
    }
    auto frame = decoder->frameAt(t - previews.length * Rational(static_cast<std::int64_t>(index)));
    if (!frame) {
        return std::nullopt;
    }
    const auto& seq = m_snapshot->sequence;
    auto composite = std::make_shared<render::CompositeFrame>();
    composite->width = seq.settings.width;
    composite->height = seq.settings.height;
    render::Layer layer;
    layer.frame = frame;
    render::coverCanvas(layer, seq.settings.width, seq.settings.height);
    layer.transform.opacity = 100.0;
    composite->layers.push_back(std::move(layer));
    if (auto caption = captionLayer(t)) {
        composite->layers.push_back(std::move(*caption));
    }
    return TimedFrame{t, m_frameDuration, frame, std::move(composite)};
}

std::optional<TimedFrame> SequenceVideoSource::multicamGrid(const Rational& t) {
    const auto& seq = m_snapshot->sequence;
    // The topmost multicam clip under the playhead.
    const model::Clip* clip = nullptr;
    const model::MediaItem* media = nullptr;
    for (auto track = seq.videoTracks.rbegin(); track != seq.videoTracks.rend() && clip == nullptr; ++track) {
        for (const auto& c : track->clips) {
            const auto m = m_snapshot->media.find(c.media);
            if (c.start <= t && t < c.end() && m != m_snapshot->media.end() && m->second.multicam) {
                clip = &c;
                media = &m->second;
                break;
            }
        }
    }
    if (clip == nullptr) {
        return std::nullopt;
    }
    const auto nested = m_snapshot->nested.find(media->sequence);
    if (nested == m_snapshot->nested.end() || !nested->second) {
        return std::nullopt;
    }
    if (m_gridMedia != media->id) {
        m_gridSources.clear();
        m_gridMedia = media->id;
    }
    const int angles = std::min<int>(9, static_cast<int>(nested->second->sequence.videoTracks.size()));
    const int columns = angles <= 1 ? 1 : angles <= 4 ? 2 : 3;
    const int rows = (angles + columns - 1) / columns;
    const double cellW = static_cast<double>(seq.settings.width) / columns;
    const double cellH = static_cast<double>(seq.settings.height) / rows;
    auto composite = std::make_shared<render::CompositeFrame>();
    composite->width = seq.settings.width;
    composite->height = seq.settings.height;
    const Rational sourceTime = clip->toSourceTime(t);
    for (int i = 0; i < angles; ++i) {
        auto& source = m_gridSources[i];
        if (!source) {
            auto snapshot = std::make_shared<SequenceSnapshot>(*nested->second);
            snapshot->useProxies = m_snapshot->useProxies;
            // Small decodes: each angle fills a cell.
            source = std::make_unique<SequenceVideoSource>(std::move(snapshot), m_allowHardware,
                                                           std::max(64, m_maxWidth > 0 ? m_maxWidth / columns : 640),
                                                           std::max(36, m_maxHeight > 0 ? m_maxHeight / rows : 360), 2, false);
            source->setTransparent(true);
            source->setSoloTrack(i);
        }
        render::Layer layer;
        layer.nested = source->compositeAt(sourceTime);
        if (!layer.nested) {
            continue;
        }
        layer.sourceWidth = layer.nested->width;
        layer.sourceHeight = layer.nested->height;
        const double scale = std::min(cellW / layer.sourceWidth, cellH / layer.sourceHeight);
        layer.transform.scale = scale * 100.0;
        layer.transform.positionX = cellW * (i % columns + 0.5);
        layer.transform.positionY = cellH * (i / columns + 0.5);
        layer.transform.anchorX = layer.sourceWidth / 2.0;
        layer.transform.anchorY = layer.sourceHeight / 2.0;
        layer.transform.opacity = i == clip->angle ? 100.0 : 70.0; // the live angle stands out
        composite->layers.push_back(std::move(layer));
    }
    return TimedFrame{t, m_frameDuration, nullptr, std::move(composite)};
}

void SequenceVideoSource::seek(const Rational& t) {
    m_next = quantizeToFrame(max(Rational(), t), m_snapshot->sequence.settings.frameRate, Rounding::Floor);
}

std::optional<TimedFrame> SequenceVideoSource::nextFrame() {
    auto f = compose(m_next, m_parallelPlayback);
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
    for (auto& [id, nested] : m_nested) {
        nested->setMaxSize(maxWidth, maxHeight);
    }
    m_reverseRuns.clear();
}

// --- SequenceAudioSource -----------------------------------------------------------------------

namespace {

std::shared_ptr<SequenceSnapshot> snapshotOf(const model::Project& project, model::SequenceId id, std::uint64_t generation,
                                             int depth) {
    const auto* seq = project.findSequence(id);
    constexpr int kMaxDepth = 16; // the model forbids cycles; this only bounds the work
    if (seq == nullptr || depth > kMaxDepth) {
        return nullptr;
    }
    auto snapshot = std::make_shared<SequenceSnapshot>();
    snapshot->sequence = *seq;
    for (const auto* list : {&seq->videoTracks, &seq->audioTracks}) {
        for (const auto& track : *list) {
            for (const auto& clip : track.clips) {
                if (snapshot->media.contains(clip.media)) {
                    continue;
                }
                const auto* media = project.findMedia(clip.media);
                if (media == nullptr) {
                    continue;
                }
                snapshot->media.emplace(clip.media, *media);
                if (media->isNest() && !snapshot->nested.contains(media->sequence)) {
                    snapshot->nested.emplace(media->sequence, snapshotOf(project, media->sequence, generation, depth + 1));
                }
            }
        }
    }
    snapshot->generation = generation;
    return snapshot;
}

bool sameNested(const SequenceSnapshot& a, const SequenceSnapshot& b) {
    if (a.nested.size() != b.nested.size()) {
        return false;
    }
    for (const auto& [id, nested] : a.nested) {
        const auto other = b.nested.find(id);
        if (other == b.nested.end() || !nested != !other->second) {
            return false;
        }
        if (nested && (nested->sequence != other->second->sequence || nested->media != other->second->media ||
                       !sameNested(*nested, *other->second))) {
            return false;
        }
    }
    return true;
}

} // namespace

std::shared_ptr<SequenceSnapshot> makeSnapshot(const model::Project& project, model::SequenceId id,
                                               std::uint64_t generation) {
    return snapshotOf(project, id, generation, 0);
}

bool sameExceptAudioMix(const SequenceSnapshot& a, const SequenceSnapshot& b) {
    if (a.media != b.media || !sameNested(a, b)) {
        return false;
    }
    const auto normalized = [](model::Sequence seq) {
        seq.masterVolumeDb = 0.0;
        for (auto& track : seq.audioTracks) {
            track.state.volumeDb = 0.0;
            track.state.pan = 0.0;
            track.state.muted = false;
            track.state.solo = false;
            track.state.volumeAutomation.clear();
            track.state.panAutomation.clear();
            track.transitions.clear(); // crossfades only change gains
            for (auto& clip : track.clips) {
                clip.fadeIn = {};
                clip.fadeOut = {};
                // Clip audio effects read their parameters every block (D38): changing them needs no reload.
                std::erase_if(clip.effects, [](const model::Effect& e) {
                    return e.type == model::kVolumeEffect || e.type == model::kPannerEffect || e.type == model::kEqEffect ||
                           e.type == model::kCompressorEffect || e.type == model::kNoiseReductionEffect;
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
    if (!reader.source) {
        try {
            if (media.isNest()) {
                const auto nested = m_snapshot->nested.find(media.sequence);
                if (nested == m_snapshot->nested.end() || !nested->second) {
                    throw std::runtime_error("nested sequence missing");
                }
                reader.source = std::make_unique<SequenceAudioSource>(nested->second, m_rate, m_channels);
            } else {
                reader.source = std::make_unique<FileAudioSource>(media.path, m_rate, m_channels);
            }
        } catch (const std::exception& e) {
            spdlog::warn("Audio offline for playback ({}): {}", model::pathToUtf8(media.path), e.what());
            m_offline.insert(clip.media);
            m_readers.erase(clip.id);
            return;
        }
    }
    const auto ch = static_cast<std::size_t>(m_channels);
    const auto count = static_cast<std::size_t>(segEnd - segStart);
    m_scratch.assign(count * ch, 0.0f);
    std::int64_t got = 0;
    const bool jumped = reader.nextFrame != segStart;
    if (clip.isRetimed()) {
        got = static_cast<std::int64_t>(readRetimed(reader, clip, segStart, count));
        applyClipEffects(reader, clip, count, jumped);
    } else {
        if (jumped) {
            reader.source->seek(clip.toSourceTime(Rational(segStart, m_rate)));
            applyClipEffects(reader, clip, 0, true);
            // Noise reduction delays its output: feed it that much audio first so the result lines
            // up with the picture (the source then reads that far ahead).
            if (hasNoiseReduction(clip)) {
                std::vector<float> preroll(audio::NoiseReducer::latency() * ch, 0.0f);
                (void)reader.source->read(preroll.data(), audio::NoiseReducer::latency());
                std::swap(m_scratch, preroll);
                applyClipEffects(reader, clip, audio::NoiseReducer::latency(), false);
                std::swap(m_scratch, preroll);
            }
        }
        got = static_cast<std::int64_t>(reader.source->read(m_scratch.data(), count));
        if (hasNoiseReduction(clip)) {
            got = static_cast<std::int64_t>(count); // the delayed tail keeps sounding past the source end
        }
        applyClipEffects(reader, clip, count, false);
    }
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

void SequenceAudioSource::ensureWindow(ClipReader& reader, std::int64_t lo, std::int64_t hi, bool backwards) {
    const auto ch = static_cast<std::int64_t>(m_channels);
    const std::int64_t have = static_cast<std::int64_t>(reader.window.size()) / ch;
    if (lo >= reader.windowStart && hi <= reader.windowStart + have) {
        return;
    }
    const std::int64_t chunk = std::max<std::int64_t>(hi - lo, m_rate / 2);
    const auto readInto = [&](std::int64_t from, std::int64_t frames) {
        const auto old = reader.window.size();
        reader.window.resize(old + static_cast<std::size_t>(frames * ch), 0.0f);
        const auto got = static_cast<std::int64_t>(reader.source->read(reader.window.data() + old, static_cast<std::size_t>(frames)));
        reader.window.resize(old + static_cast<std::size_t>(got * ch)); // past the end: shorter
        reader.readPos = from + got;
        return got;
    };
    const std::int64_t windowEnd = reader.windowStart + have;
    if (!backwards && reader.readPos == windowEnd && lo >= reader.windowStart && lo <= windowEnd) {
        // Forward and contiguous: keep reading, drop what is behind `lo`.
        const auto drop = static_cast<std::size_t>((lo - reader.windowStart) * ch);
        reader.window.erase(reader.window.begin(), reader.window.begin() + static_cast<std::ptrdiff_t>(drop));
        reader.windowStart = lo;
        if (readInto(windowEnd, std::max<std::int64_t>(hi - windowEnd, chunk)) == 0) {
            reader.readPos = -1; // end of the source: the next call re-seeks
        }
        return;
    }
    // Jump: forwards, read from `lo`; backwards, read a chunk ending at `hi` (later blocks go lower).
    const std::int64_t start = backwards ? std::max<std::int64_t>(0, hi - chunk) : lo;
    reader.window.clear();
    reader.windowStart = start;
    reader.source->seek(Rational(start, m_rate));
    readInto(start, std::max<std::int64_t>(hi - start, chunk));
}

bool SequenceAudioSource::hasNoiseReduction(const model::Clip& clip) {
    const auto* e = model::findEffectByType(clip, model::kNoiseReductionEffect);
    return e != nullptr && e->enabled;
}

void SequenceAudioSource::applyClipEffects(ClipReader& reader, const model::Clip& clip, std::size_t frames, bool reset) {
    if (reset || !reader.effectsPrimed) {
        reader.eq.reset();
        reader.compressor.reset();
        reader.noise.reset();
        reader.effectsPrimed = true;
    }
    if (frames == 0) {
        return;
    }
    // Static parameters (the EQ gains may be animated: evaluated once per block).
    const Rational t = clip.toSourceTime(clip.start);
    const auto value = [&](std::string_view type, const char* key, double fallback) {
        const auto* e = model::findEffectByType(clip, type);
        const auto* p = e != nullptr ? e->findParameter(key) : nullptr;
        return static_cast<float>(p != nullptr ? p->valueAt(t) : fallback);
    };
    float* data = m_scratch.data();
    if (const auto* e = model::findEffectByType(clip, model::kEqEffect); e != nullptr && e->enabled) {
        audio::EqParams p;
        p.lowGainDb = value(model::kEqEffect, "lowGain", 0);
        p.lowFreq = value(model::kEqEffect, "lowFreq", 100);
        p.midGainDb = value(model::kEqEffect, "midGain", 0);
        p.midFreq = value(model::kEqEffect, "midFreq", 1000);
        p.midQ = value(model::kEqEffect, "midQ", 1);
        p.highGainDb = value(model::kEqEffect, "highGain", 0);
        p.highFreq = value(model::kEqEffect, "highFreq", 8000);
        reader.eq.setParams(p, m_rate);
        reader.eq.process(data, frames, m_channels);
    }
    if (const auto* e = model::findEffectByType(clip, model::kCompressorEffect); e != nullptr && e->enabled) {
        audio::CompressorParams p;
        p.thresholdDb = value(model::kCompressorEffect, "threshold", -20);
        p.ratio = value(model::kCompressorEffect, "ratio", 4);
        p.attackMs = value(model::kCompressorEffect, "attack", 10);
        p.releaseMs = value(model::kCompressorEffect, "release", 120);
        p.makeupDb = value(model::kCompressorEffect, "makeup", 0);
        reader.compressor.setParams(p, m_rate);
        reader.compressor.process(data, frames, m_channels);
    }
    if (hasNoiseReduction(clip)) {
        reader.noise.setParams(value(model::kNoiseReductionEffect, "reduction", 12), m_channels);
        reader.noise.process(data, frames, m_channels);
    }
}

std::size_t SequenceAudioSource::readRetimed(ClipReader& reader, const model::Clip& clip, std::int64_t segStart,
                                             std::size_t count) {
    if (clip.speed.isZero()) {
        return count; // Frame Hold: silence (m_scratch is already zero)
    }
    const auto ch = static_cast<std::size_t>(m_channels);
    const double speed = clip.speed.toDouble();
    const double rate = m_rate;
    const double clipStart = clip.start.toDouble() * rate;
    const double sourceIn = clip.sourceIn.toDouble() * rate;
    const double sourceOut = clip.sourceOut().toDouble() * rate;
    // Source position (in frames at the output rate) of output frame f.
    const auto position = [&](double f) {
        const double offset = (f - clipStart) * speed;
        return clip.reverse ? sourceOut - offset : sourceIn + offset;
    };
    // Maintain Audio Pitch (D38): WSOLA grains instead of resampling.
    if (clip.maintainPitch && !clip.reverse) {
        const auto ch64 = static_cast<std::int64_t>(ch);
        const auto fetch = [&](std::int64_t pos, std::size_t frames, float* out) {
            std::fill_n(out, frames * ch, 0.0f);
            const auto lo = std::max<std::int64_t>(0, pos);
            const auto hi = pos + static_cast<std::int64_t>(frames);
            if (hi <= lo) {
                return;
            }
            // Keep some history: grains look a little back for their best fit.
            ensureWindow(reader, std::max<std::int64_t>(0, lo - 4096), hi, false);
            const auto have = static_cast<std::int64_t>(reader.window.size()) / ch64;
            for (std::int64_t f = lo; f < hi; ++f) {
                const auto i = f - reader.windowStart;
                if (i < 0 || i >= have) {
                    continue;
                }
                std::copy_n(reader.window.data() + i * ch64, ch, out + (f - pos) * ch64);
            }
        };
        reader.stretcher.render(segStart, count, m_channels,
                                [&](std::int64_t f) { return position(static_cast<double>(f)); }, fetch, m_scratch.data());
        return count;
    }
    const double p0 = position(static_cast<double>(segStart));
    const double p1 = position(static_cast<double>(segStart) + static_cast<double>(count));
    const auto lo = std::max<std::int64_t>(0, static_cast<std::int64_t>(std::floor(std::min(p0, p1))) - 1);
    const auto hi = static_cast<std::int64_t>(std::ceil(std::max(p0, p1))) + 2;
    ensureWindow(reader, lo, hi, clip.reverse);
    const auto have = static_cast<std::int64_t>(reader.window.size() / ch);
    if (have < 2) {
        return count;
    }
    for (std::size_t i = 0; i < count; ++i) {
        const double p = position(static_cast<double>(segStart) + static_cast<double>(i)) - static_cast<double>(reader.windowStart);
        const auto i0 = static_cast<std::int64_t>(std::floor(p));
        if (i0 < 0 || i0 + 1 >= have) {
            continue; // outside the decoded source (start/end of the media): silence
        }
        const auto frac = static_cast<float>(p - static_cast<double>(i0));
        const float* a = reader.window.data() + static_cast<std::size_t>(i0) * ch;
        const float* b = a + ch;
        for (std::size_t c = 0; c < ch; ++c) {
            m_scratch[i * ch + c] = a[c] + (b[c] - a[c]) * frac;
        }
    }
    return count;
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
                    const auto media = m_snapshot->media.find(clip.media);
                    std::optional<Rational> limit;
                    if (media != m_snapshot->media.end() && media->second.info.duration.isPositive() &&
                        !media->second.isGenerated()) {
                        limit = media->second.info.duration;
                    }
                    const auto room = clip.roomBefore(limit);
                    const auto earliest = room ? timeToFrames(clip.start - *room, rate, Rounding::Ceil) : clipStart;
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
        if (audible && (!track.state.volumeAutomation.empty() || !track.state.panAutomation.empty())) {
            // Track automation (D39): the fader follows its keyframes, ramped every kGainStep frames.
            const auto gainAt = [&](std::int64_t frame) {
                const Rational at(frame, m_rate);
                return audio::balanceGains(track.state.panAt(at)) * audio::dbToGain(track.state.volumeAt(at));
            };
            audio::StereoGains g = (inserted || !m_rampsValid) ? gainAt(blockStart) : from;
            for (std::int64_t f = blockStart; f < blockEnd; f += kGainStep) {
                const auto stepEnd = std::min(blockEnd, f + kGainStep);
                const auto next = gainAt(stepEnd);
                audio::applyRamp(m_bus.data() + static_cast<std::size_t>(f - blockStart) * ch,
                                 static_cast<std::size_t>(stepEnd - f), m_channels, g, next);
                g = next;
            }
            prev->second = g;
        } else {
            audio::applyRamp(m_bus.data(), nFrames, m_channels, from, target);
        }
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
