#include "playback/playback_clock.h"

#include <algorithm>

namespace loki::playback {

void PlaybackClock::start(const Rational& position, int rate, bool audioDriven, int sampleRate,
                          std::int64_t latencyFrames) {
    std::lock_guard lock(m_mutex);
    m_anchor = position;
    m_anchorWall = m_now();
    m_rate = rate;
    m_running = rate != 0;
    m_audioDriven = audioDriven && rate == 1;
    m_sampleRate = std::max(1, sampleRate);
    m_latency = std::max<std::int64_t>(0, latencyFrames);
    m_audioFrames.store(0, std::memory_order_relaxed);
}

void PlaybackClock::pause() {
    const Rational t = now();
    std::lock_guard lock(m_mutex);
    m_anchor = t;
    m_running = false;
    m_rate = 0;
    m_audioDriven = false;
}

void PlaybackClock::setPosition(const Rational& position) {
    std::lock_guard lock(m_mutex);
    m_anchor = position;
    m_anchorWall = m_now();
    m_audioFrames.store(0, std::memory_order_relaxed);
}

Rational PlaybackClock::now() const {
    std::lock_guard lock(m_mutex);
    if (!m_running) {
        return m_anchor;
    }
    if (m_audioDriven) {
        // Frames handed to the device but not yet audible do not count.
        const auto audible = std::max<std::int64_t>(0, m_audioFrames.load(std::memory_order_relaxed) - m_latency);
        return m_anchor + Rational(audible, m_sampleRate);
    }
    const auto elapsedUs =
        std::chrono::duration_cast<std::chrono::microseconds>(m_now() - m_anchorWall).count();
    return m_anchor + Rational(elapsedUs, 1'000'000) * Rational(m_rate);
}

bool PlaybackClock::isRunning() const {
    std::lock_guard lock(m_mutex);
    return m_running;
}

int PlaybackClock::rate() const {
    std::lock_guard lock(m_mutex);
    return m_rate;
}

bool PlaybackClock::isAudioDriven() const {
    std::lock_guard lock(m_mutex);
    return m_audioDriven;
}

int nextShuttleRate(int currentRate, ShuttleKey key) noexcept {
    switch (key) {
    case ShuttleKey::K: return 0;
    case ShuttleKey::L: return currentRate <= 0 ? 1 : std::min(currentRate * 2, 8);
    case ShuttleKey::J: return currentRate >= 0 ? -1 : std::max(currentRate * 2, -8);
    }
    return 0;
}

} // namespace loki::playback
