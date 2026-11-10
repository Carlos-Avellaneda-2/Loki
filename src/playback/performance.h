#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace loki::playback {

// What this computer has (used to size caches and threads).
struct HardwareInfo {
    unsigned threads = 1;       // logical processors
    std::uint64_t ramBytes = 0; // physical memory (0 = unknown)
};
[[nodiscard]] HardwareInfo detectHardware();

// Playback performance (Preferences > Performance). Standard is tuned for a 2-core laptop; High uses
// more memory and threads on bigger machines (more layers in real time, longer caches).
enum class PerformanceMode { Auto, Standard, High };

struct PerformanceProfile {
    PerformanceMode mode = PerformanceMode::Standard; // resolved (never Auto)
    std::size_t frameCacheBytes = 384u * 1024u * 1024u; // decoded frames (Source Monitor)
    int lookAheadFrames = 8;                           // frames decoded ahead while playing
    std::size_t maxVideoDecoders = 8;                  // video layers decodable without re-opening
    std::size_t compositeCacheEntries = 48;            // recently shown sequence pictures
    bool parallelPlaybackDecode = false;               // decode layers of a frame on several cores
};

// Auto picks High with at least 8 threads and 12 GB of RAM, Standard otherwise.
[[nodiscard]] PerformanceProfile performanceProfile(PerformanceMode mode, const HardwareInfo& hardware);

[[nodiscard]] const char* performanceModeName(PerformanceMode mode) noexcept;

} // namespace loki::playback
