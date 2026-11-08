#include "util/task_pool.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <exception>

namespace loki::util {

TaskPool::TaskPool(std::size_t threads, std::string name) : m_name(std::move(name)) {
    threads = std::max<std::size_t>(1, threads);
    m_workers.reserve(threads);
    for (std::size_t i = 0; i < threads; ++i) {
        m_workers.emplace_back([this](const std::stop_token& stop) { run(stop); });
    }
}

TaskPool::~TaskPool() {
    {
        std::lock_guard lock(m_mutex);
        m_stopping = true; // tasks still running may try to submit more: drop those
        while (!m_queue.empty()) {
            m_queue.top().token.m_cancelled->store(true);
            m_queue.pop();
        }
    }
    for (auto& w : m_workers) {
        w.request_stop();
    }
    m_wake.notify_all();
    m_workers.clear(); // joins
}

TaskHandle TaskPool::submit(Task task, TaskPriority priority) {
    CancelToken token;
    TaskHandle handle(token.m_cancelled);
    {
        std::lock_guard lock(m_mutex);
        if (m_stopping) {
            token.m_cancelled->store(true);
            return handle;
        }
        m_queue.push(Entry{static_cast<int>(priority), m_nextSequence++, std::move(task), token});
    }
    m_wake.notify_one();
    return handle;
}

void TaskPool::run(const std::stop_token& stop) {
    while (true) {
        Entry entry;
        {
            std::unique_lock lock(m_mutex);
            if (!m_wake.wait(lock, stop, [this] { return !m_queue.empty(); })) {
                return; // stop requested and nothing queued
            }
            entry = m_queue.top();
            m_queue.pop();
            ++m_running;
        }
        if (!entry.token.cancelled()) {
            try {
                entry.task(entry.token);
            } catch (const std::exception& e) {
                spdlog::error("[{}] task threw: {}", m_name, e.what());
            } catch (...) {
                spdlog::error("[{}] task threw an unknown exception", m_name);
            }
        }
        {
            std::lock_guard lock(m_mutex);
            --m_running;
            if (m_running == 0 && m_queue.empty()) {
                m_idle.notify_all();
            }
        }
    }
}

void TaskPool::waitIdle() {
    std::unique_lock lock(m_mutex);
    m_idle.wait(lock, [this] { return m_running == 0 && m_queue.empty(); });
}

std::size_t TaskPool::pendingCount() const {
    std::lock_guard lock(m_mutex);
    return m_queue.size();
}

std::size_t TaskPool::defaultDecodeThreads() noexcept {
    const auto hw = std::thread::hardware_concurrency();
    return hw > 3 ? hw - 2 : 1;
}

} // namespace loki::util
