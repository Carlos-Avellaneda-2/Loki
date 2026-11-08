#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace loki::util {

// Cancellation flag shared between a submitted task and its handle.
class CancelToken {
public:
    [[nodiscard]] bool cancelled() const noexcept { return m_cancelled->load(std::memory_order_acquire); }

private:
    friend class TaskHandle;
    friend class TaskPool;
    std::shared_ptr<std::atomic<bool>> m_cancelled = std::make_shared<std::atomic<bool>>(false);
};

// Lets the submitter cancel a task. Cancelling a task that has not started skips it; a running
// task must poll CancelToken::cancelled() to stop early.
class TaskHandle {
public:
    TaskHandle() = default;
    void cancel() const noexcept {
        if (m_cancelled) {
            m_cancelled->store(true, std::memory_order_release);
        }
    }
    [[nodiscard]] bool valid() const noexcept { return m_cancelled != nullptr; }

private:
    friend class TaskPool;
    explicit TaskHandle(std::shared_ptr<std::atomic<bool>> flag) : m_cancelled(std::move(flag)) {}
    std::shared_ptr<std::atomic<bool>> m_cancelled;
};

enum class TaskPriority : int { Background = 0, Normal = 1, High = 2, Interactive = 3 };

// Fixed-size worker pool with priorities (higher first, FIFO within a priority) and cooperative
// cancellation. Destruction cancels pending tasks and joins the workers.
class TaskPool {
public:
    using Task = std::function<void(const CancelToken&)>;

    explicit TaskPool(std::size_t threads, std::string name = "pool");
    ~TaskPool();
    TaskPool(const TaskPool&) = delete;
    TaskPool& operator=(const TaskPool&) = delete;
    TaskPool(TaskPool&&) = delete;
    TaskPool& operator=(TaskPool&&) = delete;

    TaskHandle submit(Task task, TaskPriority priority = TaskPriority::Normal);

    // Blocks until the queue is empty and no task is running (tests, shutdown).
    void waitIdle();

    [[nodiscard]] std::size_t threadCount() const noexcept { return m_workers.size(); }
    [[nodiscard]] std::size_t pendingCount() const;

    // Default worker count for decode work: hardware threads minus two (UI + audio), at least 1.
    [[nodiscard]] static std::size_t defaultDecodeThreads() noexcept;

private:
    struct Entry {
        int priority;
        std::uint64_t sequence;
        Task task;
        CancelToken token;
        bool operator<(const Entry& other) const noexcept {
            // std::priority_queue pops the largest: higher priority first, then lower sequence.
            return priority != other.priority ? priority < other.priority : sequence > other.sequence;
        }
    };

    void run(const std::stop_token& stop);

    mutable std::mutex m_mutex;
    std::condition_variable_any m_wake;
    std::condition_variable m_idle;
    std::priority_queue<Entry> m_queue;
    std::uint64_t m_nextSequence = 0;
    std::size_t m_running = 0;
    bool m_stopping = false;
    std::string m_name;
    std::vector<std::jthread> m_workers;
};

} // namespace loki::util
