#include "media/frame_cache.h"

namespace loki::media {

VideoFramePtr FrameCache::get(const FrameKey& key) {
    std::lock_guard lock(m_mutex);
    const auto it = m_index.find(key);
    if (it == m_index.end()) {
        ++m_misses;
        return nullptr;
    }
    ++m_hits;
    m_lru.splice(m_lru.begin(), m_lru, it->second);
    return it->second->second;
}

void FrameCache::put(const FrameKey& key, VideoFramePtr frame) {
    if (!frame) {
        return;
    }
    const auto size = frame->byteSize();
    std::lock_guard lock(m_mutex);
    if (size > m_maxBytes) {
        return; // would evict everything and still not fit
    }
    if (const auto it = m_index.find(key); it != m_index.end()) {
        m_bytes -= it->second->second->byteSize();
        it->second->second = std::move(frame);
        m_bytes += size;
        m_lru.splice(m_lru.begin(), m_lru, it->second);
    } else {
        m_lru.emplace_front(key, std::move(frame));
        m_index.emplace(key, m_lru.begin());
        m_bytes += size;
    }
    evictLocked();
}

void FrameCache::evictLocked() {
    while (m_bytes > m_maxBytes && !m_lru.empty()) {
        const auto& victim = m_lru.back();
        m_bytes -= victim.second->byteSize();
        m_index.erase(victim.first);
        m_lru.pop_back();
    }
}

void FrameCache::clear() {
    std::lock_guard lock(m_mutex);
    m_lru.clear();
    m_index.clear();
    m_bytes = 0;
}

void FrameCache::setMaxBytes(std::size_t maxBytes) {
    std::lock_guard lock(m_mutex);
    m_maxBytes = maxBytes;
    evictLocked();
}

std::size_t FrameCache::bytes() const {
    std::lock_guard lock(m_mutex);
    return m_bytes;
}

std::size_t FrameCache::size() const {
    std::lock_guard lock(m_mutex);
    return m_lru.size();
}

std::uint64_t FrameCache::hits() const {
    std::lock_guard lock(m_mutex);
    return m_hits;
}

std::uint64_t FrameCache::misses() const {
    std::lock_guard lock(m_mutex);
    return m_misses;
}

} // namespace loki::media
