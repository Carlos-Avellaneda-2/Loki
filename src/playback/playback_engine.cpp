#include "playback/playback_engine.h"

#include "media/frame_cache.h"
#include "model/project_json.h"
#include "util/spsc_ring.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>

namespace loki::playback {

namespace {

constexpr int kAudioRingMs = 500;
constexpr int kAudioRefillFrames = 1024;
constexpr int kMeterWindowMs = 50;
// Video further behind the clock than this is skipped by re-seeking instead of decoding through.
const Rational kResyncThreshold(1, 2);

} // namespace

PlaybackEngine::PlaybackEngine(PlaybackOptions options)
    : m_options(std::move(options)),
      m_clock(m_options.timeSource ? m_options.timeSource : TimeSource(&std::chrono::steady_clock::now)),
      m_cache(std::make_unique<media::FrameCache>(m_options.cacheBytes)) {
    auto callback = [this](float* out, std::size_t frames) { audioCallback(out, frames); };
    m_device = m_options.deviceFactory ? m_options.deviceFactory(callback) : audio::createDefaultDevice(callback);
    if (m_device) {
        m_ring = std::make_unique<util::SpscRing<float>>(static_cast<std::size_t>(m_device->sampleRate()) *
                                                         static_cast<std::size_t>(m_device->channels()) *
                                                         kAudioRingMs / 1000);
        m_audioScratch.resize(static_cast<std::size_t>(kAudioRefillFrames * m_device->channels()));
        m_device->start();
    }
    m_thread = std::jthread([this](const std::stop_token& stop) { run(stop); });
}

PlaybackEngine::~PlaybackEngine() {
    m_thread.request_stop();
    m_wake.notify_all();
    if (m_thread.joinable()) {
        m_thread.join();
    }
    if (m_device) {
        m_device->stop();
    }
}

// --- UI-thread API ---------------------------------------------------------------------------

void PlaybackEngine::open(const std::filesystem::path& path, const model::MediaInfo& info) {
    {
        std::lock_guard lock(m_requestMutex);
        m_requests = {};
        m_requests.open = FileRequest{path, info};
        m_pending = true;
    }
    m_requestedRate = 0;
    m_wake.notify_all();
}

void PlaybackEngine::openSequence(std::shared_ptr<const SequenceSnapshot> snapshot) {
    {
        std::lock_guard lock(m_requestMutex);
        m_requests.open = std::move(snapshot); // keeps pending seek/rate requests
        m_requests.close = false;
        m_pending = true;
    }
    m_wake.notify_all();
}

void PlaybackEngine::close() {
    {
        std::lock_guard lock(m_requestMutex);
        m_requests = {};
        m_requests.close = true;
        m_pending = true;
    }
    m_requestedRate = 0;
    m_wake.notify_all();
}

std::string PlaybackEngine::lastError() const {
    std::lock_guard lock(m_infoMutex);
    return m_error;
}

void PlaybackEngine::setRate(int rate) {
    rate = std::clamp(rate, -8, 8);
    m_requestedRate = rate;
    {
        std::lock_guard lock(m_requestMutex);
        m_requests.rate = rate;
        m_pending = true;
    }
    m_wake.notify_all();
}

void PlaybackEngine::seek(const Rational& t) {
    {
        std::lock_guard lock(m_requestMutex);
        m_requests.seek = t; // latest wins
        m_pending = true;
    }
    m_wake.notify_all();
}

void PlaybackEngine::stepFrames(int frames) {
    Rational base;
    {
        std::lock_guard lock(m_displayMutex);
        base = m_current ? m_current->time : position();
    }
    const Rational rate = frameRate();
    const Rational target = base + Rational(frames) / rate + rate.reciprocal() / Rational(2);
    m_requestedRate = 0;
    {
        std::lock_guard lock(m_requestMutex);
        m_requests.rate = 0;
        m_requests.seek = clampTime(quantizeToFrame(target, rate, Rounding::Floor));
        m_pending = true;
    }
    m_wake.notify_all();
}

void PlaybackEngine::setQuality(int maxWidth, int maxHeight) {
    {
        std::lock_guard lock(m_requestMutex);
        m_requests.quality = std::make_pair(maxWidth, maxHeight);
        m_pending = true;
    }
    m_wake.notify_all();
}

Rational PlaybackEngine::position() const {
    return clampTime(m_clock.now());
}

Rational PlaybackEngine::duration() const {
    std::lock_guard lock(m_infoMutex);
    return m_duration;
}

Rational PlaybackEngine::frameRate() const {
    std::lock_guard lock(m_infoMutex);
    return m_frameRate;
}

Rational PlaybackEngine::clampTime(const Rational& t) const {
    Rational d;
    Rational rate;
    bool sequence = false;
    {
        std::lock_guard lock(m_infoMutex);
        d = m_duration;
        rate = m_frameRate;
        sequence = m_sequenceMode;
    }
    if (sequence) {
        return max(Rational(), t); // the timeline playhead may go past the last clip
    }
    const Rational last = d.isPositive() ? max(Rational(), d - rate.reciprocal()) : Rational();
    return min(max(Rational(), t), last);
}

std::int64_t PlaybackEngine::frameIndexOf(const Rational& t) const {
    return timeToFrames(t, frameRate(), Rounding::Floor);
}

media::VideoFramePtr PlaybackEngine::currentFrame() {
    const Rational t = m_clock.now();
    const int r = m_clock.rate();
    std::lock_guard lock(m_displayMutex);
    const auto discard = [this] {
        const auto& front = m_queue.front();
        const bool shown = (m_current && m_current->time == front.time) ||
                           (m_lastCountedTime && *m_lastCountedTime == front.time);
        if (!shown) {
            ++m_dropped;
        }
        m_queue.pop_front();
    };
    if (r > 0) {
        while (m_queue.size() > 1 && m_queue.front().time + m_queue.front().duration <= t) {
            discard();
        }
        if (!m_queue.empty() && m_queue.front().time <= t) {
            m_current = m_queue.front();
        }
    } else if (r < 0) {
        while (m_queue.size() > 1 && m_queue.front().time > t) {
            discard();
        }
        if (!m_queue.empty() && m_queue.front().time <= t + m_queue.front().duration) {
            m_current = m_queue.front();
        }
    }
    if (m_current && (!m_lastCountedTime || *m_lastCountedTime != m_current->time)) {
        m_lastCountedTime = m_current->time;
        ++m_shown;
    }
    return m_current ? m_current->frame : nullptr;
}

Rational PlaybackEngine::currentFrameTime() const {
    std::lock_guard lock(m_displayMutex);
    return m_current ? m_current->time : Rational();
}

std::optional<TimedFrame> PlaybackEngine::currentPicture() const {
    std::lock_guard lock(m_displayMutex);
    return m_current;
}

PlaybackStats PlaybackEngine::stats() const {
    PlaybackStats s;
    s.framesShown = m_shown;
    s.framesDropped = m_dropped;
    s.audioUnderruns = m_underruns;
    s.lastSeekMs = m_lastSeekMs;
    std::lock_guard lock(m_infoMutex);
    s.hwAccel = m_hwName;
    return s;
}

bool PlaybackEngine::waitIdle(int timeoutMs) {
    std::unique_lock lock(m_requestMutex);
    return m_idle.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this] { return !m_pending && !m_busy; });
}

// --- Audio thread ----------------------------------------------------------------------------

void PlaybackEngine::audioCallback(float* out, std::size_t frames) noexcept {
    const auto channels = static_cast<std::size_t>(m_device->channels());
    const std::size_t samples = frames * channels;
    if (!m_audioActive.load(std::memory_order_acquire)) {
        std::fill_n(out, samples, 0.0f);
    } else {
        const std::size_t got = m_ring->read(out, samples);
        if (got < samples) {
            std::fill_n(out + got, samples - got, 0.0f);
            m_underruns.fetch_add(1, std::memory_order_relaxed);
        }
        m_clock.onAudioFramesPlayed(static_cast<std::int64_t>(frames));
    }
    m_callbacks.fetch_add(1, std::memory_order_release); // at the end: "this callback is done"
}

// --- Media thread ----------------------------------------------------------------------------

void PlaybackEngine::run(const std::stop_token& stop) {
    while (!stop.stop_requested()) {
        Requests requests;
        bool have = false;
        {
            std::unique_lock lock(m_requestMutex);
            const auto wait = m_activeRate != 0 ? std::chrono::milliseconds(3) : std::chrono::milliseconds(50);
            m_wake.wait_for(lock, stop, wait, [this] { return m_pending; });
            if (stop.stop_requested()) {
                break;
            }
            if (m_pending) {
                requests = std::move(m_requests);
                m_requests = {};
                m_pending = false;
                m_busy = true;
                have = true;
            }
        }
        try {
            if (have) {
                process(std::move(requests));
            }
            if (m_activeRate != 0 && (m_video || m_audio)) {
                feedAudio();
                if (m_activeRate > 0) {
                    fillForward();
                } else {
                    fillReverse();
                }
                checkEnd();
            }
        } catch (const std::exception& e) {
            spdlog::error("Playback error: {}", e.what());
            {
                std::lock_guard lock(m_infoMutex);
                m_error = e.what();
            }
            activateAudio(false);
            m_clock.pause();
            m_activeRate = 0;
            m_requestedRate = 0;
        }
        {
            std::lock_guard lock(m_requestMutex);
            m_busy = false;
            if (!m_pending) {
                m_idle.notify_all();
            }
        }
    }
    activateAudio(false);
}

void PlaybackEngine::process(Requests requests) {
    if (requests.close) {
        doClose();
        return;
    }
    if (const auto* file = std::get_if<FileRequest>(&requests.open)) {
        doOpenFile(*file);
    } else if (const auto* snapshot = std::get_if<std::shared_ptr<const SequenceSnapshot>>(&requests.open)) {
        doOpenSequence(*snapshot);
    }
    if (!m_video && !m_audio) {
        return;
    }
    if (requests.quality) {
        m_maxWidth = requests.quality->first;
        m_maxHeight = requests.quality->second;
        if (m_video) {
            m_video->setMaxSize(m_maxWidth, m_maxHeight);
        }
        m_cache->clear();
        m_compositeCache.clear();
        if (!requests.seek) {
            requests.seek = m_clock.now();
        }
    }
    if (requests.rate || requests.seek) {
        const int rate = requests.rate.value_or(m_activeRate);
        const Rational position = clampTime(requests.seek.value_or(m_clock.now()));
        applyTransport(rate, position);
    }
}

void PlaybackEngine::doClose() {
    activateAudio(false);
    m_clock.pause();
    m_clock.setPosition(Rational());
    m_activeRate = 0;
    m_video.reset();
    m_audio.reset();
    m_snapshot.reset();
    m_cache->clear();
    m_compositeCache.clear();
    clearQueue();
    {
        std::lock_guard lock(m_displayMutex);
        m_current.reset();
    }
    std::lock_guard lock(m_infoMutex);
    m_duration = Rational();
    m_error.clear();
    m_hwName.clear();
    m_sequenceMode = false;
    m_open = false;
}

void PlaybackEngine::doOpenFile(const FileRequest& request) {
    doClose();
    std::string error;
    Rational decoderRate;
    try {
        if (request.info.hasVideo) {
            auto video = std::make_unique<FileVideoSource>(request.path, m_options.allowHardware && !request.info.isStill,
                                                           m_maxWidth, m_maxHeight);
            decoderRate = video->frameRate();
            m_video = std::move(video);
        }
        if (request.info.hasAudio && m_device) {
            m_audio = std::make_unique<FileAudioSource>(request.path, m_device->sampleRate(), m_device->channels());
        }
    } catch (const std::exception& e) {
        error = e.what();
        m_video.reset();
        m_audio.reset();
    }
    {
        std::lock_guard lock(m_infoMutex);
        m_error = error;
        Rational rate = request.info.frameRate.isPositive() ? request.info.frameRate : Rational(30);
        if (decoderRate.isPositive()) {
            rate = decoderRate;
        }
        m_frameRate = rate;
        m_duration = request.info.isStill ? Rational() : request.info.duration;
        m_hwName = m_video ? m_video->hwAccel() : "";
        m_sequenceMode = false;
    }
    m_cacheSource = model::pathToUtf8(request.path);
    m_videoEof = false;
    const bool ok = (m_video || m_audio) && error.empty();
    if (ok) {
        showFrameAt(Rational());
    } else {
        spdlog::warn("Cannot open {} for playback: {}", m_cacheSource, error);
    }
    m_open = ok;
    ++m_openCount;
}

void PlaybackEngine::doOpenSequence(std::shared_ptr<const SequenceSnapshot> snapshot) {
    const bool update = m_snapshot && snapshot && m_snapshot->sequence.id == snapshot->sequence.id;
    if (update && sameExceptAudioMix(*m_snapshot, *snapshot)) {
        // Mixer move (fader, pan, mute, clip volume, fade): keep decoders, buffers and pictures.
        // The ring still holds up to kAudioRingMs of audio mixed with the old settings.
        if (auto* audio = dynamic_cast<SequenceAudioSource*>(m_audio.get())) {
            audio->updateMix(snapshot);
        }
        m_snapshot = std::move(snapshot);
        return;
    }
    const Rational position = update ? m_clock.now() : Rational();
    const int rate = update ? m_activeRate : 0;
    if (!update) {
        doClose();
    } else {
        activateAudio(false);
        clearQueue();
    }
    m_snapshot = std::move(snapshot);
    m_compositeCache.clear();
    if (!m_snapshot) {
        return;
    }
    m_video = std::make_unique<SequenceVideoSource>(m_snapshot, m_options.allowHardware, m_maxWidth, m_maxHeight);
    if (m_device) {
        m_audio = std::make_unique<SequenceAudioSource>(m_snapshot, m_device->sampleRate(), m_device->channels());
    }
    {
        std::lock_guard lock(m_infoMutex);
        m_frameRate = m_snapshot->sequence.settings.frameRate;
        m_duration = m_snapshot->sequence.duration();
        m_sequenceMode = true;
        m_error.clear();
    }
    m_cacheSource = "sequence:" + m_snapshot->sequence.id.toString() + ":" + std::to_string(m_snapshot->generation);
    m_open = true;
    ++m_openCount;
    applyTransport(rate, position); // redisplays with the new content, keeps playing if playing
}

void PlaybackEngine::applyTransport(int rate, const Rational& position) {
    activateAudio(false);
    clearQueue();
    m_videoEof = false;
    const auto start = std::chrono::steady_clock::now();
    m_clock.pause();
    m_clock.setPosition(position);
    showFrameAt(position);
    m_lastSeekMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();

    m_activeRate = rate;
    if (rate == 0) {
        return;
    }
    if (rate > 0 && m_video) {
        m_video->seek(position); // decoding continues from the displayed picture
    }
    if (rate < 0) {
        m_reverseNext = position;
        m_reverseDone = false;
    }
    const bool audioDriven = rate == 1 && m_audio && m_device;
    if (audioDriven) {
        m_audio->seek(position);
        m_audioWriteFrame = timeToFrames(position, Rational(m_device->sampleRate()), Rounding::Nearest);
        feedAudio(); // prime before the clock starts
        m_clock.start(position, rate, true, m_device->sampleRate(), static_cast<std::int64_t>(m_device->latencyFrames()));
        activateAudio(true);
    } else {
        m_clock.start(position, rate, false);
    }
}

namespace {
constexpr std::size_t kCompositeCacheEntries = 48;
} // namespace

void PlaybackEngine::cachePut(const TimedFrame& f) {
    if (!f.frame) {
        return;
    }
    if (m_snapshot) {
        const auto index = frameIndexOf(f.time);
        std::erase_if(m_compositeCache, [index](const auto& e) { return e.first == index; });
        m_compositeCache.emplace_front(index, f);
        if (m_compositeCache.size() > kCompositeCacheEntries) {
            m_compositeCache.pop_back();
        }
        return;
    }
    m_cache->put({m_cacheSource, m_maxWidth, frameIndexOf(f.time)}, f.frame);
}

std::optional<TimedFrame> PlaybackEngine::cacheGet(const Rational& t) {
    const auto index = frameIndexOf(t);
    if (m_snapshot) {
        for (auto it = m_compositeCache.begin(); it != m_compositeCache.end(); ++it) {
            if (it->first == index) {
                m_compositeCache.splice(m_compositeCache.begin(), m_compositeCache, it);
                return m_compositeCache.front().second;
            }
        }
        return std::nullopt;
    }
    auto frame = m_cache->get({m_cacheSource, m_maxWidth, index});
    if (!frame) {
        return std::nullopt;
    }
    return TimedFrame{frame->pts, frame->duration, frame, nullptr};
}

void PlaybackEngine::showFrameAt(const Rational& t) {
    if (!m_video) {
        return;
    }
    auto picture = cacheGet(t);
    if (!picture) {
        picture = m_video->frameAt(t);
        if (picture) {
            cachePut(*picture);
        }
    }
    std::lock_guard lock(m_displayMutex);
    if (picture) {
        m_current = std::move(*picture);
    } else {
        // Past the end of a sequence: black at the playhead.
        m_current = TimedFrame{t, frameRate().reciprocal(), nullptr};
    }
}

void PlaybackEngine::fillForward() {
    if (!m_video || m_videoEof) {
        return;
    }
    const Rational now = m_clock.now();
    std::size_t queued = 0;
    Rational lastQueued(-1);
    {
        std::lock_guard lock(m_displayMutex);
        queued = m_queue.size();
        if (!m_queue.empty()) {
            lastQueued = m_queue.back().time;
        }
    }
    // Far behind (slow decode, heavy file): jump to the clock instead of decoding through.
    if (!lastQueued.isNegative() && lastQueued + kResyncThreshold < now) {
        clearQueue();
        m_video->seek(now);
        ++m_dropped;
    }
    while (queued < static_cast<std::size_t>(m_options.lookAheadFrames)) {
        auto picture = m_video->nextFrame();
        if (!picture) {
            m_videoEof = true;
            return;
        }
        cachePut(*picture);
        if (picture->time + picture->duration <= now) {
            ++m_dropped; // already late: never queue it
            continue;
        }
        std::lock_guard lock(m_displayMutex);
        m_queue.push_back(std::move(*picture));
        queued = m_queue.size();
    }
}

void PlaybackEngine::fillReverse() {
    if (!m_video || m_reverseDone) {
        return;
    }
    std::size_t queued = 0;
    {
        std::lock_guard lock(m_displayMutex);
        queued = m_queue.size();
    }
    const Rational frameDur = frameRate().reciprocal();
    while (queued < static_cast<std::size_t>(m_options.lookAheadFrames) && !m_reverseDone) {
        // Cache first (scrubbing back and forth), otherwise decode a run up to the target.
        std::vector<TimedFrame> run;
        if (auto cached = cacheGet(m_reverseNext)) {
            run.push_back(std::move(*cached));
        } else {
            run = m_video->framesUpTo(m_reverseNext);
            for (const auto& f : run) {
                cachePut(f);
            }
        }
        if (run.empty()) {
            m_reverseDone = true;
            break;
        }
        std::lock_guard lock(m_displayMutex);
        for (auto it = run.rbegin(); it != run.rend(); ++it) {
            if (it->time <= m_reverseNext) {
                m_queue.push_back(*it);
            }
        }
        queued = m_queue.size();
        m_reverseNext = run.front().time - frameDur;
        if (m_reverseNext.isNegative()) {
            m_reverseDone = true;
        }
    }
}

void PlaybackEngine::feedAudio() {
    if (!m_audio || !m_ring || m_activeRate != 1) {
        return;
    }
    const auto channels = static_cast<std::size_t>(m_device->channels());
    while (m_ring->freeSpace() >= m_audioScratch.size()) {
        const std::size_t frames = m_audio->read(m_audioScratch.data(), m_audioScratch.size() / channels);
        if (frames == 0) {
            break; // end of audio: the callback plays silence while the clock keeps running
        }
        m_ring->write(m_audioScratch.data(), frames * channels);
        MeterBlock block{m_audioWriteFrame, static_cast<std::int64_t>(frames),
                         audio::measure(m_audioScratch.data(), frames, static_cast<int>(channels)),
                         m_audio->trackLevels()};
        m_audioWriteFrame += static_cast<std::int64_t>(frames);
        std::lock_guard lock(m_meterMutex);
        m_meterBlocks.push_back(std::move(block));
        // Keep about two ring lengths of history.
        const auto keep = static_cast<std::int64_t>(m_device->sampleRate()) * kAudioRingMs * 2 / 1000;
        while (!m_meterBlocks.empty() && m_meterBlocks.front().start + keep < m_audioWriteFrame) {
            m_meterBlocks.pop_front();
        }
    }
}

void PlaybackEngine::activateAudio(bool on) {
    if (!m_device || !m_ring) {
        return;
    }
    if (on) {
        m_audioActive.store(true, std::memory_order_release);
        return;
    }
    if (!m_audioActive.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    // Wait until any callback that may still read the ring has finished, then drain it from here
    // (safe: an inactive callback never touches the ring).
    if (m_device->isRunning() && m_device->name() != "manual") {
        const auto seen = m_callbacks.load(std::memory_order_acquire);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
        while (m_callbacks.load(std::memory_order_acquire) < seen + 2 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    m_ring->clearFromConsumer();
    std::lock_guard lock(m_meterMutex);
    m_meterBlocks.clear();
}

MixLevels PlaybackEngine::levels() const {
    MixLevels out;
    if (!m_device || !m_audioActive.load(std::memory_order_acquire)) {
        return out;
    }
    const auto now = timeToFrames(max(Rational(), m_clock.now()), Rational(m_device->sampleRate()), Rounding::Floor);
    const auto window = static_cast<std::int64_t>(m_device->sampleRate()) * kMeterWindowMs / 1000;
    std::lock_guard lock(m_meterMutex);
    std::size_t combined = 0;
    for (const auto& block : m_meterBlocks) {
        const auto blockEnd = block.start + block.frames;
        // Blocks heard during the last kMeterWindowMs (covers a ~30 Hz UI poll without gaps).
        const bool heard = block.start <= now && blockEnd > now - window;
        if (!heard) {
            continue;
        }
        const auto frames = static_cast<std::size_t>(block.frames);
        if (combined == 0) {
            out.master = block.master;
            out.tracks = block.tracks;
        } else {
            out.master = audio::StereoLevel::combine(out.master, combined, block.master, frames);
            for (std::size_t i = 0; i < out.tracks.size() && i < block.tracks.size(); ++i) {
                if (out.tracks[i].track == block.tracks[i].track) {
                    out.tracks[i].level =
                        audio::StereoLevel::combine(out.tracks[i].level, combined, block.tracks[i].level, frames);
                }
            }
        }
        combined += frames;
    }
    out.active = true;
    return out;
}

void PlaybackEngine::checkEnd() {
    Rational d;
    bool sequence = false;
    {
        std::lock_guard lock(m_infoMutex);
        d = m_duration;
        sequence = m_sequenceMode;
    }
    const Rational now = m_clock.now();
    const Rational last = sequence ? d : max(Rational(), d - frameRate().reciprocal());
    bool ended = false;
    if (m_activeRate > 0 && now >= d) {
        ended = true;
    } else if (m_activeRate < 0 && now.isNegative()) {
        ended = true;
    }
    if (ended) {
        m_requestedRate = 0;
        applyTransport(0, m_activeRate > 0 ? last : Rational());
    }
}

void PlaybackEngine::clearQueue() {
    std::lock_guard lock(m_displayMutex);
    m_queue.clear();
}

} // namespace loki::playback
