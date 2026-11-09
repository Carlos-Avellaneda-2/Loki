#pragma once

#include "model/types.h"

#include <atomic>
#include <filesystem>
#include <functional>
#include <string>
#include <utility>

namespace loki::exporter {

// Proxies (D24): H.264, at most 1280x720 (aspect kept, even size), keyframe every 15 frames, video
// only (sound always comes from the original). Preview-only: export reads the originals.
inline constexpr int kProxyMaxWidth = 1280;
inline constexpr int kProxyMaxHeight = 720;
inline constexpr int kProxyGop = 15;

[[nodiscard]] std::pair<int, int> proxySize(int width, int height);
[[nodiscard]] std::filesystem::path proxyPathFor(const std::filesystem::path& cacheRoot, model::MediaId id);
// Whether a proxy makes sense (file video that is not a still image).
[[nodiscard]] bool canHaveProxy(const model::MediaItem& media);

struct ProxyResult {
    bool ok = false;
    bool cancelled = false;
    std::string error;
};

// Transcodes `media` into `output` (written to a temporary name and renamed when complete). Runs
// on the calling thread; `progress` (0..1) is called now and then.
[[nodiscard]] ProxyResult makeProxy(const model::MediaItem& media, const std::filesystem::path& output,
                                    const std::atomic<bool>* cancel = nullptr,
                                    const std::function<void(double)>& progress = {});

} // namespace loki::exporter
