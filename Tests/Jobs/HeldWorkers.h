#pragma once

#include "JobSystem/WorkStealingThreadPool.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <limits>
#include <thread>

namespace GameEngine::Tests
{

// Yields until `pred` holds or `timeout` passes; false on the timeout.
inline bool WaitUntil(const std::function<bool()>& pred, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!pred())
    {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::yield();
    }
    return true;
}

// Holds `workers` workers of the pool (every worker by default) inside one
// task each until Release(), which returns once every holding task has stopped
// touching this object, so it may be destroyed before the pool. A pool that
// shuts down lets its held workers go, since its shutdown joins them; they
// then take no more work. Destroying it releases the workers it still holds,
// so a test that returns early on a failed assertion does not leave the pool
// held.
class HeldWorkers
{
  public:
    static constexpr size_t kEveryWorker = std::numeric_limits<size_t>::max();

    explicit HeldWorkers(JobSystem::WorkStealingThreadPool& pool, size_t workers = kEveryWorker)
    {
        if (workers == kEveryWorker)
            workers = pool.GetWorkerCount();
        for (size_t i = 0; i < workers; ++i)
        {
            pool.EnqueueWork([this, &pool] {
                m_Holding.fetch_add(1);
                while (!m_Released.load() && !pool.IsShuttingDown())
                    std::this_thread::yield();
                m_Holding.fetch_sub(1);
            });
        }
        EXPECT_TRUE(WaitUntil([this, workers] { return m_Holding.load() == workers; }, std::chrono::seconds(10)));
    }
    ~HeldWorkers() { Release(); }

    HeldWorkers(const HeldWorkers&) = delete;
    HeldWorkers& operator=(const HeldWorkers&) = delete;

    void Release()
    {
        m_Released.store(true);
        EXPECT_TRUE(WaitUntil([this] { return m_Holding.load() == 0; }, std::chrono::seconds(10)));
    }

  private:
    std::atomic<size_t> m_Holding{0};
    std::atomic<bool> m_Released{false};
};

} // namespace GameEngine::Tests
