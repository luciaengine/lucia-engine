#include <gtest/gtest.h>

#include "Platform/Thread.h"

#include <atomic>
#include <cstdint>
#include <thread>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#endif

using namespace GameEngine;

namespace
{
// The id GetThreadName takes: what the Memory module's scratch stats record per thread.
[[maybe_unused]] std::uint64_t ThisOsThreadId()
{
#if defined(_WIN32)
    return ::GetCurrentThreadId();
#elif defined(__APPLE__)
    std::uint64_t id = 0;
    pthread_threadid_np(nullptr, &id);
    return id;
#elif defined(__linux__)
    return static_cast<std::uint64_t>(::syscall(SYS_gettid));
#else
    return 0;
#endif
}
} // namespace

// The debug server names another thread by the OS id the Memory stats carry.
TEST(PlatformThread, GetThreadNameReadsAnotherThreadsName)
{
#if (!defined(_WIN32) && !defined(__APPLE__) && !defined(__linux__)) || (defined(__APPLE__) && TARGET_OS_IPHONE)
    GTEST_SKIP() << "Reading another thread name is unavailable on this platform";
#else
    std::atomic<std::uint64_t> id{0};
    std::atomic<bool> done{false};
    std::thread named([&id, &done] {
        Platform::SetCurrentThreadName("Platform Probe");
        id.store(ThisOsThreadId(), std::memory_order_release);
        while (!done.load(std::memory_order_acquire))
            std::this_thread::yield();
    });
    while (id.load(std::memory_order_acquire) == 0)
        std::this_thread::yield();
    const std::string name = Platform::GetThreadName(id.load());
    done.store(true, std::memory_order_release);
    named.join();
    EXPECT_EQ(name, "Platform Probe");
#endif
}
