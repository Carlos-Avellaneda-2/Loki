#include "playback/performance.h"

#include <algorithm>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace loki::playback {

HardwareInfo detectHardware() {
    HardwareInfo hw;
    hw.threads = std::max(1u, std::thread::hardware_concurrency());
#ifdef _WIN32
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status) != 0) {
        hw.ramBytes = status.ullTotalPhys;
    }
#else
    const long pages = sysconf(_SC_PHYS_PAGES);
    const long size = sysconf(_SC_PAGE_SIZE);
    if (pages > 0 && size > 0) {
        hw.ramBytes = static_cast<std::uint64_t>(pages) * static_cast<std::uint64_t>(size);
    }
#endif
    return hw;
}

PerformanceProfile performanceProfile(PerformanceMode mode, const HardwareInfo& hw) {
    constexpr std::uint64_t kGB = 1024ull * 1024ull * 1024ull;
    if (mode == PerformanceMode::Auto) {
        mode = (hw.threads >= 8 && hw.ramBytes >= 12 * kGB) ? PerformanceMode::High : PerformanceMode::Standard;
    }
    PerformanceProfile p;
    p.mode = mode;
    if (mode == PerformanceMode::High) {
        // An eighth of the RAM for decoded frames, between 512 MB and 4 GB.
        const std::uint64_t cache = hw.ramBytes > 0 ? hw.ramBytes / 8 : 1024ull * 1024ull * 1024ull;
        p.frameCacheBytes = static_cast<std::size_t>(std::clamp<std::uint64_t>(cache, 512ull << 20, 4 * kGB));
        p.lookAheadFrames = 16;
        p.maxVideoDecoders = 32;
        p.compositeCacheEntries = 120;
        p.parallelPlaybackDecode = true;
    }
    return p;
}

const char* performanceModeName(PerformanceMode mode) noexcept {
    switch (mode) {
    case PerformanceMode::Auto: return "Auto";
    case PerformanceMode::Standard: return "Standard";
    case PerformanceMode::High: return "High";
    }
    return "Standard";
}

} // namespace loki::playback
