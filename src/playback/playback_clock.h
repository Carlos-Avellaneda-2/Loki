#pragma once

#include "util/rational.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>

namespace loki::playback {

using TimeSource = std::function<std::chrono::steady_clock::time_point()>;

// Media clock for playback. At 1x with an audio device the audio hardware is the master: time
// advances with the frames the device actually consumed (minus its output latency), so video
// follows sound. Otherwise (no audio, shuttle speeds, reverse) a monotonic wall clock scaled by
// the rate is used. Thread-safe; onAudioFramesPlayed() is lock-free (audio callback).
class PlaybackClock {
public:
    explicit PlaybackClock(TimeSource now = &std::chrono::steady_clock::now) : m_now(std::move(now)) {}

    // Starts running from `position` at `rate` (e.g. -2 = reverse double speed).
    // audioDriven: time comes from onAudioFramesPlayed() at `sampleRate`, delayed by `latencyFrames`.
    void start(const Rational& position, int rate, bool audioDriven, int sampleRate = 48000,
               std::int64_t latencyFrames = 0);
    // Freezes at the current time.
    void pause();
    // Sets the time while paused (seek/scrub).
    void setPosition(const Rational& position);

    void onAudioFramesPlayed(std::int64_t frames) noexcept { m_audioFrames.fetch_add(frames, std::memory_order_relaxed); }

    [[nodiscard]] Rational now() const;
    [[nodiscard]] bool isRunning() const;
    [[nodiscard]] int rate() const;
    [[nodiscard]] bool isAudioDriven() const;

private:
    TimeSource m_now;
    mutable std::mutex m_mutex;
    Rational m_anchor;
    std::chrono::steady_clock::time_point m_anchorWall;
    int m_rate = 0;
    bool m_running = false;
    bool m_audioDriven = false;
    int m_sampleRate = 48000;
    std::int64_t m_latency = 0;
    std::atomic<std::int64_t> m_audioFrames{0};
};

// J/K/L shuttle (Premiere behaviour): L plays forward and doubles the speed on each press up to
// 8x (from reverse it switches to 1x forward); J mirrors it backwards; K stops.
enum class ShuttleKey { J, K, L };
[[nodiscard]] int nextShuttleRate(int currentRate, ShuttleKey key) noexcept;

} // namespace loki::playback
