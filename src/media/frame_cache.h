#pragma once

#include "media/video_frame.h"

#include <cstddef>
#include <cstdint>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>

namespace loki::media {

struct FrameKey {
    std::string source; // file path (or media id) + decode variant
    int quality = 0;    // max width bucket; frames of different sizes are different entries
    std::int64_t frame = 0;

    friend bool operator==(const FrameKey&, const FrameKey&) = default;
};

struct FrameKeyHash {
    std::size_t operator()(const FrameKey& k) const noexcept {
        auto h = std::hash<std::string>{}(k.source);
        h ^= std::hash<std::int64_t>{}(k.frame) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        h ^= std::hash<int>{}(k.quality) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        return h;
    }
};

// Thread-safe LRU cache of decoded frames bounded by total bytes.
class FrameCache {
public:
    explicit FrameCache(std::size_t maxBytes) : m_maxBytes(maxBytes) {}

    [[nodiscard]] VideoFramePtr get(const FrameKey& key);
    void put(const FrameKey& key, VideoFramePtr frame);
    void clear();
    void setMaxBytes(std::size_t maxBytes);

    [[nodiscard]] std::size_t bytes() const;
    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] std::uint64_t hits() const;
    [[nodiscard]] std::uint64_t misses() const;

private:
    using Entry = std::pair<FrameKey, VideoFramePtr>;

    void evictLocked();

    mutable std::mutex m_mutex;
    std::size_t m_maxBytes;
    std::size_t m_bytes = 0;
    std::list<Entry> m_lru; // front = most recently used
    std::unordered_map<FrameKey, std::list<Entry>::iterator, FrameKeyHash> m_index;
    std::uint64_t m_hits = 0;
    std::uint64_t m_misses = 0;
};

} // namespace loki::media
