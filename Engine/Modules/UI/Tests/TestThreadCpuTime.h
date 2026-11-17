#pragma once

#include <chrono>
#include <cstddef>
#include <stdexcept>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

namespace GameEngine::UI::Tests
{

inline std::chrono::nanoseconds ReadThreadCpuTime()
{
#ifdef _WIN32
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user))
        throw std::runtime_error("GetThreadTimes failed");
    ULARGE_INTEGER kernelTicks{}, userTicks{};
    kernelTicks.LowPart = kernel.dwLowDateTime;
    kernelTicks.HighPart = kernel.dwHighDateTime;
    userTicks.LowPart = user.dwLowDateTime;
    userTicks.HighPart = user.dwHighDateTime;
    using FileTimeDuration = std::chrono::duration<long long, std::ratio<1, 10'000'000>>;
    return FileTimeDuration(kernelTicks.QuadPart + userTicks.QuadPart);
#else
    timespec time{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &time) != 0)
        throw std::runtime_error("clock_gettime(CLOCK_THREAD_CPUTIME_ID) failed");
    return std::chrono::seconds(time.tv_sec) + std::chrono::nanoseconds(time.tv_nsec);
#endif
}

// Measure work on the calling thread. Collect enough samples for coarse platform clocks.
template <typename Work>
double MeanThreadCpuMilliseconds(Work&& work)
{
    constexpr auto kMinimumCpuTime = std::chrono::milliseconds(125);
    constexpr std::size_t kMaxSamples = 65'536;
    const auto start = ReadThreadCpuTime();
    std::chrono::nanoseconds elapsed{};
    std::size_t samples = 0;
    do
    {
        work();
        ++samples;
        elapsed = ReadThreadCpuTime() - start;
    } while (elapsed < kMinimumCpuTime && samples < kMaxSamples);
    if (elapsed <= std::chrono::nanoseconds::zero())
        throw std::runtime_error("Thread CPU time did not advance during the test");
    return std::chrono::duration<double, std::milli>(elapsed).count() / static_cast<double>(samples);
}

} // namespace GameEngine::UI::Tests
