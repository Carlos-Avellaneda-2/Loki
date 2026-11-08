#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <memory>
#include <new>
#include <type_traits>

namespace loki::util {

// Lock-free single-producer / single-consumer ring buffer of trivially copyable items.
// Safe for real-time threads: no locks, no allocation after construction. Used to feed the
// audio callback.
template <typename T>
class SpscRing {
    static_assert(std::is_trivially_copyable_v<T>);

public:
    explicit SpscRing(std::size_t capacity) : m_capacity(capacity + 1), m_buffer(std::make_unique<T[]>(capacity + 1)) {}
    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;
    SpscRing(SpscRing&&) = delete;
    SpscRing& operator=(SpscRing&&) = delete;
    ~SpscRing() = default;

    [[nodiscard]] std::size_t capacity() const noexcept { return m_capacity - 1; }

    // Producer side. Returns how many items were written (may be < count when full).
    std::size_t write(const T* data, std::size_t count) noexcept {
        const auto head = m_head.load(std::memory_order_relaxed);
        const auto tail = m_tail.load(std::memory_order_acquire);
        const auto free = (tail + m_capacity - head - 1) % m_capacity;
        const auto n = std::min(count, free);
        const auto first = std::min(n, m_capacity - head);
        std::copy_n(data, first, m_buffer.get() + head);
        std::copy_n(data + first, n - first, m_buffer.get());
        m_head.store((head + n) % m_capacity, std::memory_order_release);
        return n;
    }

    // Consumer side. Returns how many items were read (may be < count when empty).
    std::size_t read(T* out, std::size_t count) noexcept {
        const auto tail = m_tail.load(std::memory_order_relaxed);
        const auto head = m_head.load(std::memory_order_acquire);
        const auto available = (head + m_capacity - tail) % m_capacity;
        const auto n = std::min(count, available);
        const auto first = std::min(n, m_capacity - tail);
        std::copy_n(m_buffer.get() + tail, first, out);
        std::copy_n(m_buffer.get(), n - first, out + first);
        m_tail.store((tail + n) % m_capacity, std::memory_order_release);
        return n;
    }

    // Approximate from either side.
    [[nodiscard]] std::size_t size() const noexcept {
        const auto head = m_head.load(std::memory_order_acquire);
        const auto tail = m_tail.load(std::memory_order_acquire);
        return (head + m_capacity - tail) % m_capacity;
    }
    [[nodiscard]] std::size_t freeSpace() const noexcept { return capacity() - size(); }

    // Consumer side: drops everything currently buffered.
    void clearFromConsumer() noexcept { m_tail.store(m_head.load(std::memory_order_acquire), std::memory_order_release); }

private:
    const std::size_t m_capacity;
    std::unique_ptr<T[]> m_buffer;
    alignas(64) std::atomic<std::size_t> m_head{0}; // next write position (producer)
    alignas(64) std::atomic<std::size_t> m_tail{0}; // next read position (consumer)
};

} // namespace loki::util
