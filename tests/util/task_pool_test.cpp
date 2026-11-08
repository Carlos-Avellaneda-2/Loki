#include "util/spsc_ring.h"
#include "util/task_pool.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

using namespace loki::util;

TEST(TaskPoolTest, RunsAllTasks) {
    TaskPool pool(3);
    std::atomic<int> count{0};
    for (int i = 0; i < 100; ++i) {
        pool.submit([&](const CancelToken&) { ++count; });
    }
    pool.waitIdle();
    EXPECT_EQ(count, 100);
    EXPECT_EQ(pool.pendingCount(), 0u);
}

TEST(TaskPoolTest, HigherPriorityRunsFirstFifoWithin) {
    TaskPool pool(1);
    std::mutex m;
    std::vector<int> order;
    std::atomic<bool> release{false};
    pool.submit([&](const CancelToken&) {
        while (!release) {
            std::this_thread::yield();
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20)); // blocker is running
    const auto record = [&](int v) {
        return [&, v](const CancelToken&) {
            std::lock_guard lock(m);
            order.push_back(v);
        };
    };
    pool.submit(record(1), TaskPriority::Background);
    pool.submit(record(2), TaskPriority::Normal);
    pool.submit(record(3), TaskPriority::Interactive);
    pool.submit(record(4), TaskPriority::Normal);
    release = true;
    pool.waitIdle();
    EXPECT_EQ(order, (std::vector<int>{3, 2, 4, 1}));
}

TEST(TaskPoolTest, CancelledTasksAreSkipped) {
    TaskPool pool(1);
    std::atomic<bool> release{false};
    std::atomic<int> ran{0};
    pool.submit([&](const CancelToken&) {
        while (!release) {
            std::this_thread::yield();
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    auto handle = pool.submit([&](const CancelToken&) { ++ran; });
    EXPECT_TRUE(handle.valid());
    handle.cancel();
    release = true;
    pool.waitIdle();
    EXPECT_EQ(ran, 0);
}

TEST(TaskPoolTest, RunningTaskSeesCancellation) {
    TaskPool pool(1);
    std::atomic<bool> started{false};
    std::atomic<bool> stoppedEarly{false};
    auto handle = pool.submit([&](const CancelToken& token) {
        started = true;
        for (int i = 0; i < 5000; ++i) {
            if (token.cancelled()) {
                stoppedEarly = true;
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    while (!started) {
        std::this_thread::yield();
    }
    handle.cancel();
    pool.waitIdle();
    EXPECT_TRUE(stoppedEarly);
}

TEST(TaskPoolTest, ExceptionsDoNotKillWorkers) {
    TaskPool pool(1);
    std::atomic<int> ran{0};
    pool.submit([](const CancelToken&) { throw std::runtime_error("boom"); });
    pool.submit([&](const CancelToken&) { ++ran; });
    pool.waitIdle();
    EXPECT_EQ(ran, 1);
}

TEST(TaskPoolTest, DestructionCancelsPendingWork) {
    std::atomic<int> ran{0};
    {
        TaskPool pool(1);
        for (int i = 0; i < 1000; ++i) {
            pool.submit([&](const CancelToken&) {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
                ++ran;
            });
        }
    }
    EXPECT_LT(ran, 1000);
}

TEST(TaskPoolTest, DefaultThreadCountIsPositive) {
    EXPECT_GE(TaskPool::defaultDecodeThreads(), 1u);
}

TEST(SpscRingTest, WriteReadWrapAround) {
    SpscRing<int> ring(5);
    EXPECT_EQ(ring.capacity(), 5u);
    const int in[] = {1, 2, 3, 4};
    EXPECT_EQ(ring.write(in, 4), 4u);
    int out[8] = {};
    EXPECT_EQ(ring.read(out, 3), 3u);
    EXPECT_EQ(out[0], 1);
    EXPECT_EQ(out[2], 3);
    const int more[] = {5, 6, 7, 8, 9};
    EXPECT_EQ(ring.write(more, 5), 4u); // only 4 free
    EXPECT_EQ(ring.size(), 5u);
    EXPECT_EQ(ring.freeSpace(), 0u);
    EXPECT_EQ(ring.read(out, 8), 5u);
    EXPECT_EQ(out[0], 4);
    EXPECT_EQ(out[4], 8);
    EXPECT_EQ(ring.read(out, 1), 0u);
}

TEST(SpscRingTest, ClearFromConsumer) {
    SpscRing<float> ring(16);
    const float data[10] = {};
    ring.write(data, 10);
    ring.clearFromConsumer();
    EXPECT_EQ(ring.size(), 0u);
}

TEST(SpscRingTest, ConcurrentProducerConsumerKeepsOrder) {
    SpscRing<std::uint32_t> ring(256);
    constexpr std::uint32_t kCount = 200000;
    std::thread producer([&] {
        std::uint32_t next = 0;
        while (next < kCount) {
            std::uint32_t chunk[37];
            std::uint32_t n = 0;
            while (n < 37 && next + n < kCount) {
                chunk[n] = next + n;
                ++n;
            }
            next += static_cast<std::uint32_t>(ring.write(chunk, n));
        }
    });
    std::uint32_t expected = 0;
    bool ordered = true;
    while (expected < kCount) {
        std::uint32_t buf[64];
        const auto n = ring.read(buf, 64);
        for (std::size_t i = 0; i < n; ++i) {
            ordered = ordered && buf[i] == expected;
            ++expected;
        }
    }
    producer.join();
    EXPECT_TRUE(ordered);
}

} // namespace
