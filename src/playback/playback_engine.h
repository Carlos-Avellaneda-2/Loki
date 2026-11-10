#pragma once

#include "audio/audio_device.h"
#include "media/video_frame.h"
#include "model/types.h"
#include "playback/playback_clock.h"
#include "playback/sources.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <list>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <variant>

namespace loki::media {
class FrameCache;
} // namespace loki::media

namespace loki::util {
template <typename T>
class SpscRing;
} // namespace loki::util

namespace loki::playback {

struct PlaybackOptions {
    audio::AudioDeviceFactory deviceFactory; // null: default miniaudio device
    TimeSource timeSource;                   // null: steady_clock
    std::size_t cacheBytes = 384u * 1024u * 1024u;
    bool allowHardware = true;
    int lookAheadFrames = 8;
    // Sized by the performance profile (playback/performance.h).
    std::size_t maxVideoDecoders = 8;
    std::size_t compositeCacheEntries = 48;
    bool parallelPlaybackDecode = false;
};

// Levels heard now (meters). Empty/inactive while not playing at 1x.
struct MixLevels {
    bool active = false;
    audio::StereoLevel master;
    std::vector<TrackLevel> tracks; // sequence mode: one entry per audio track
};

struct PlaybackStats {
    std::uint64_t framesShown = 0;
    std::uint64_t framesDropped = 0;
    std::uint64_t audioUnderruns = 0;
    double lastSeekMs = 0;
    std::string hwAccel;
};

// Plays a media file (Source Monitor) or a sequence snapshot (Program Monitor). All decoding runs
// on an internal media thread; the UI thread only posts requests and polls currentFrame() and
// position(), so it never blocks. A/V sync: at 1x the audio device is the master clock; pictures
// are picked by that clock and late ones are dropped instead of drifting.
class PlaybackEngine {
public:
    explicit PlaybackEngine(PlaybackOptions options = {});
    ~PlaybackEngine();
    PlaybackEngine(const PlaybackEngine&) = delete;
    PlaybackEngine& operator=(const PlaybackEngine&) = delete;
    PlaybackEngine(PlaybackEngine&&) = delete;
    PlaybackEngine& operator=(PlaybackEngine&&) = delete;

    // Asynchronous. `info` comes from the project (probe); errors are reported by lastError().
    // `videoPath` (proxy, D24) replaces `path` for pictures only; sound always comes from `path`.
    void open(const std::filesystem::path& path, const model::MediaInfo& info,
              const std::filesystem::path& videoPath = {});
    // Plays a sequence. Calling it again with a newer snapshot of the same sequence keeps the
    // position and transport state (live update after edits).
    void openSequence(std::shared_ptr<const SequenceSnapshot> snapshot);
    void close();
    [[nodiscard]] bool isOpen() const noexcept { return m_open.load(); }
    // Increments every time an open()/openSequence() completes (successfully or not).
    [[nodiscard]] std::uint64_t openCount() const noexcept { return m_openCount.load(); }
    [[nodiscard]] std::string lastError() const;

    void play() { setRate(1); }
    void pause() { setRate(0); }
    void togglePlay() { setRate(rate() == 0 ? 1 : 0); }
    void setRate(int rate); // -8..8, 0 = pause
    void shuttle(ShuttleKey key) { setRate(nextShuttleRate(rate(), key)); }
    // Scrub/seek. The latest request wins; older pending ones are dropped. Keeps playing if playing.
    void seek(const Rational& t);
    // Pauses and moves by whole frames (arrow keys).
    void stepFrames(int frames);
    // Output size bound (playback resolution: full, 1/2, 1/4). 0 = source size.
    void setQuality(int maxWidth, int maxHeight);

    [[nodiscard]] Rational position() const;
    [[nodiscard]] Rational duration() const;
    [[nodiscard]] Rational frameRate() const;
    [[nodiscard]] int rate() const noexcept { return m_requestedRate.load(); }
    [[nodiscard]] bool isPlaying() const noexcept { return rate() != 0; }

    // Pixels to display now (UI thread, never blocks on decoding). Null before the first picture,
    // for audio-only media, and over gaps of a sequence.
    [[nodiscard]] media::VideoFramePtr currentFrame();
    // Timeline time of the picture returned by the last currentFrame() call.
    [[nodiscard]] Rational currentFrameTime() const;
    // Picture selected by the last currentFrame() call, including the composite for sequences.
    [[nodiscard]] std::optional<TimedFrame> currentPicture() const;

    [[nodiscard]] PlaybackStats stats() const;
    // Peak/RMS of what was heard during the last ~50 ms (UI meters; never blocks for long).
    [[nodiscard]] MixLevels levels() const;
    [[nodiscard]] bool hasAudioOutput() const noexcept { return m_device != nullptr; }

    // Test helper: waits until pending requests were processed. Returns false on timeout.
    bool waitIdle(int timeoutMs = 5000);

private:
    struct FileRequest {
        std::filesystem::path path;
        model::MediaInfo info;
        std::filesystem::path videoPath;
    };
    struct Requests {
        std::variant<std::monostate, FileRequest, std::shared_ptr<const SequenceSnapshot>> open;
        bool close = false;
        std::optional<Rational> seek;
        std::optional<int> rate;
        std::optional<std::pair<int, int>> quality;
    };

    void run(const std::stop_token& stop);
    void process(Requests requests);
    void doOpenFile(const FileRequest& request);
    void doOpenSequence(std::shared_ptr<const SequenceSnapshot> snapshot);
    void doClose();
    void applyTransport(int rate, const Rational& position);
    void showFrameAt(const Rational& t);
    void fillForward();
    void fillReverse();
    void feedAudio();
    void activateAudio(bool on);
    void checkEnd();
    void clearQueue();
    void cachePut(const TimedFrame& f);
    [[nodiscard]] std::optional<TimedFrame> cacheGet(const Rational& t);
    [[nodiscard]] Rational clampTime(const Rational& t) const;
    [[nodiscard]] std::int64_t frameIndexOf(const Rational& t) const;
    void audioCallback(float* out, std::size_t frames) noexcept;

    PlaybackOptions m_options;
    PlaybackClock m_clock;
    std::unique_ptr<audio::AudioDevice> m_device;
    std::unique_ptr<util::SpscRing<float>> m_ring;
    std::unique_ptr<media::FrameCache> m_cache; // file mode
    std::list<std::pair<std::int64_t, TimedFrame>> m_compositeCache; // sequence mode (LRU by frame index)

    // Media-thread state.
    std::unique_ptr<VideoSource> m_video;
    std::unique_ptr<AudioSource> m_audio;
    std::shared_ptr<const SequenceSnapshot> m_snapshot; // set in sequence mode
    std::string m_cacheSource;
    int m_activeRate = 0;
    Rational m_reverseNext;
    bool m_reverseDone = false;
    bool m_videoEof = false;
    int m_maxWidth = 0;
    int m_maxHeight = 0;
    std::vector<float> m_audioScratch;
    std::int64_t m_audioWriteFrame = 0; // timeline frame (device rate) of the next block written

    struct MeterBlock {
        std::int64_t start = 0;
        std::int64_t frames = 0;
        audio::StereoLevel master;
        std::vector<TrackLevel> tracks;
    };
    mutable std::mutex m_meterMutex;
    std::deque<MeterBlock> m_meterBlocks; // ascending start

    // Shared, read by the UI thread.
    std::atomic<bool> m_open{false};
    std::atomic<std::uint64_t> m_openCount{0};
    std::atomic<int> m_requestedRate{0};
    mutable std::mutex m_infoMutex;
    Rational m_duration;
    Rational m_frameRate{30};
    bool m_sequenceMode = false; // seeks are not clamped to the duration (free playhead)
    std::string m_error;

    mutable std::mutex m_displayMutex;
    std::deque<TimedFrame> m_queue; // forward: ascending time; reverse: descending
    std::optional<TimedFrame> m_current;
    std::optional<Rational> m_lastCountedTime;

    std::atomic<bool> m_audioActive{false};
    std::atomic<std::uint64_t> m_callbacks{0};
    std::atomic<std::uint64_t> m_underruns{0};
    std::atomic<std::uint64_t> m_shown{0};
    std::atomic<std::uint64_t> m_dropped{0};
    std::atomic<double> m_lastSeekMs{0};
    std::string m_hwName;

    std::mutex m_requestMutex;
    std::condition_variable_any m_wake;
    std::condition_variable m_idle;
    Requests m_requests;
    bool m_pending = false;
    bool m_busy = false;
    std::jthread m_thread; // last: started after everything above exists, joined first
};

} // namespace loki::playback
