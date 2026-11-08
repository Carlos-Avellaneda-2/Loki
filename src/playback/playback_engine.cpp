#include "playback/playback_engine.h"

#include "media/audio_decoder.h"
#include "media/frame_cache.h"
#include "media/video_decoder.h"
#include "model/project_json.h"
#include "util/spsc_ring.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>

namespace loki::playback {

namespace {

constexpr int kAudioRingMs = 500;
constexpr int kAudioRefillFrames = 1024;
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
        m_requests.open = std::make_pair(path, info);
        m_pending = true;
    }
    m_requestedRate = 0;
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
        base = m_current ? m_current->pts : position();
    }
    const Rational target = base + Rational(frames) / frameRate() + frameRate().reciprocal() / Rational(2);
    m_requestedRate = 0;
    {
        std::lock_guard lock(m_requestMutex);
        m_requests.rate = 0;
        m_requests.seek = clampTime(quantizeToFrame(target, frameRate(), Rounding::Floor));
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
    const Rational d = duration();
    const Rational last = d.isPositive() ? max(Rational(), d - frameRate().reciprocal()) : Rational();
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
        if (m_queue.front() != m_current && m_queue.front() != m_lastCounted) {
            ++m_dropped;
        }
        m_queue.pop_front();
    };
    if (r > 0) {
        while (!m_queue.empty() && m_queue.front()->pts + m_queue.front()->duration <= t && m_queue.size() > 1) {
            discard();
        }
        if (!m_queue.empty() && m_queue.front()->pts <= t) {
            m_current = m_queue.front();
        }
    } else if (r < 0) {
        while (!m_queue.empty() && m_queue.front()->pts > t && m_queue.size() > 1) {
            discard();
        }
        if (!m_queue.empty() && m_queue.front()->pts <= t + m_queue.front()->duration) {
            m_current = m_queue.front();
        }
    }
    if (m_current && m_current != m_lastCounted) {
        m_lastCounted = m_current;
        ++m_shown;
    }
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
    if (requests.open) {
        doOpen(requests.open->first, requests.open->second);
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
    m_cache->clear();
    clearQueue();
    {
        std::lock_guard lock(m_displayMutex);
        m_current.reset();
    }
    std::lock_guard lock(m_infoMutex);
    m_duration = Rational();
    m_error.clear();
    m_hwName.clear();
    m_open = false;
}

void PlaybackEngine::doOpen(const std::filesystem::path& path, const model::MediaInfo& info) {
    doClose();
    std::string error;
    try {
        if (info.hasVideo) {
            media::VideoDecoderOptions options;
            options.allowHardware = m_options.allowHardware && !info.isStill;
            options.maxWidth = m_maxWidth;
            options.maxHeight = m_maxHeight;
            m_video = std::make_unique<media::VideoDecoder>(path, options);
        }
        if (info.hasAudio && m_device) {
            m_audio = std::make_unique<media::AudioDecoder>(path, m_device->sampleRate(), m_device->channels());
        }
    } catch (const std::exception& e) {
        error = e.what();
        m_video.reset();
        m_audio.reset();
    }
    {
        std::lock_guard lock(m_infoMutex);
        m_error = error;
        Rational rate = info.frameRate.isPositive() ? info.frameRate : Rational(30);
        if (m_video && m_video->info().frameRate.isPositive()) {
            rate = m_video->info().frameRate;
        }
        m_frameRate = rate;
        m_duration = info.isStill ? Rational() : info.duration;
        m_hwName = m_video ? media::hwAccelName(m_video->hwAccel()) : "";
    }
    m_cacheSource = model::pathToUtf8(path);
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
        m_video->seek(position); // decoding continues from the displayed frame
    }
    if (rate < 0) {
        m_reverseNext = position;
        m_reverseDone = false;
    }
    const bool audioDriven = rate == 1 && m_audio && m_device;
    if (audioDriven) {
        m_audio->seek(position);
        feedAudio(); // prime before the clock starts
        m_clock.start(position, rate, true, m_device->sampleRate(), static_cast<std::int64_t>(m_device->latencyFrames()));
        activateAudio(true);
    } else {
        m_clock.start(position, rate, false);
    }
}

void PlaybackEngine::showFrameAt(const Rational& t) {
    if (!m_video) {
        return;
    }
    const media::FrameKey key{m_cacheSource, m_maxWidth, frameIndexOf(t)};
    auto frame = m_cache->get(key);
    if (!frame) {
        frame = m_video->frameAt(t);
        m_cache->put(key, frame);
    }
    if (frame) {
        std::lock_guard lock(m_displayMutex);
        m_current = frame;
    }
}

void PlaybackEngine::fillForward() {
    if (!m_video || m_videoEof) {
        return;
    }
    const Rational now = m_clock.now();
    std::size_t queued = 0;
    Rational lastQueued;
    {
        std::lock_guard lock(m_displayMutex);
        queued = m_queue.size();
        lastQueued = m_queue.empty() ? Rational(-1) : m_queue.back()->pts;
    }
    // Far behind (slow decode, heavy file): jump to the clock instead of decoding through.
    if (!lastQueued.isNegative() && lastQueued + kResyncThreshold < now) {
        clearQueue();
        m_video->seek(now);
        ++m_dropped;
    }
    while (queued < static_cast<std::size_t>(m_options.lookAheadFrames)) {
        auto frame = m_video->nextFrame();
        if (!frame) {
            m_videoEof = true;
            return;
        }
        m_cache->put({m_cacheSource, m_maxWidth, frameIndexOf(frame->pts)}, frame);
        if (frame->pts + frame->duration <= now) {
            ++m_dropped; // already late: never queue it
            continue;
        }
        std::lock_guard lock(m_displayMutex);
        m_queue.push_back(std::move(frame));
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
        // Cache first (scrubbing back and forth), otherwise decode the whole run up to the target.
        const media::FrameKey key{m_cacheSource, m_maxWidth, frameIndexOf(m_reverseNext)};
        std::vector<media::VideoFramePtr> run;
        if (auto cached = m_cache->get(key)) {
            run.push_back(std::move(cached));
        } else {
            run = m_video->framesUpTo(m_reverseNext);
            for (const auto& f : run) {
                m_cache->put({m_cacheSource, m_maxWidth, frameIndexOf(f->pts)}, f);
            }
        }
        if (run.empty()) {
            m_reverseDone = true;
            break;
        }
        std::lock_guard lock(m_displayMutex);
        for (auto it = run.rbegin(); it != run.rend(); ++it) {
            if ((*it)->pts <= m_reverseNext) {
                m_queue.push_back(*it);
            }
        }
        queued = m_queue.size();
        m_reverseNext = run.front()->pts - frameDur;
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
}

void PlaybackEngine::checkEnd() {
    const Rational d = duration();
    if (!d.isPositive()) {
        return;
    }
    const Rational now = m_clock.now();
    const Rational last = max(Rational(), d - frameRate().reciprocal());
    bool ended = false;
    if (m_activeRate > 0 && now >= d) {
        ended = true;
    } else if (m_activeRate < 0 && now.isNegative()) {
        ended = true;
    }
    if (ended) {
        const Rational stopAt = m_activeRate > 0 ? last : Rational();
        m_requestedRate = 0;
        applyTransport(0, stopAt);
    }
}

void PlaybackEngine::clearQueue() {
    std::lock_guard lock(m_displayMutex);
    m_queue.clear();
}

} // namespace loki::playback
