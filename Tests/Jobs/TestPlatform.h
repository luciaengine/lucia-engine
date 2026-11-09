#pragma once

// Cross-platform shims for the pure JobSystem suite. The suite was born on
// Windows; its latency diagnostics (timer-resolution logging, ambient-CPU
// sampling) and spin hint are WinAPI. Windows keeps the original behavior;
// other hosts get honest fallbacks so the assertions — which are all
// platform-neutral — run everywhere.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#define GE_TEST_HAS_WIN_TIMERS 1
#else
#define GE_TEST_HAS_WIN_TIMERS 0
// WinAPI spin hint; a scheduler yield is the closest portable equivalent for
// the test-side busy waits (the pool under test has its own CpuRelax).
#define YieldProcessor() std::this_thread::yield()
#endif

namespace GameEngine::Tests
{
// Death tests: keep an expected abort from raising the Windows error-report
// UI. POSIX aborts are already silent.
inline void DisableAbortDialogs()
{
#if defined(_WIN32)
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
}

// Process-environment write, usable inside death-test bodies (preprocessor
// directives cannot appear inside macro arguments).
inline void SetEnvVar(const char* name, const char* value)
{
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

// Whole-machine CPU times. Windows: GetSystemTimes, whose kernel time
// includes idle time. Other hosts read zero, which means "unmeasured".
struct AmbientCpuSample
{
    uint64_t Idle = 0;
    uint64_t Kernel = 0;
    uint64_t User = 0;
};

inline AmbientCpuSample SampleSystemCpu()
{
    AmbientCpuSample s;
#if GE_TEST_HAS_WIN_TIMERS
    FILETIME idle{}, kernel{}, user{};
    GetSystemTimes(&idle, &kernel, &user);
    const auto pack = [](const FILETIME& ft) {
        return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    };
    s.Idle = pack(idle);
    s.Kernel = pack(kernel);
    s.User = pack(user);
#endif
    return s;
}

// Busy percent of the whole machine between two samples:
// (kernel - idle + user) / (kernel + user). Zero when nothing was measured.
inline double AmbientBusyPercent(const AmbientCpuSample& a, const AmbientCpuSample& b)
{
    const double idle = static_cast<double>(b.Idle - a.Idle);
    const double kernel = static_cast<double>(b.Kernel - a.Kernel);
    const double user = static_cast<double>(b.User - a.User);
    const double total = kernel + user;
    return total > 0.0 ? 100.0 * (total - idle) / total : 0.0;
}

// The window of the Windows machine-load sample.
constexpr auto kMachineLoadWindow = std::chrono::milliseconds(100);

// Machine load as a share of the hardware threads: 1 means one busy thread per
// hardware thread. macOS and Linux: the 1-minute load average, which can pass 1
// and lags by up to a minute. Windows: the busy share over kMachineLoadWindow.
// A failed read gives 0.
inline double ReadMachineLoad()
{
#if GE_TEST_HAS_WIN_TIMERS
    const AmbientCpuSample before = SampleSystemCpu();
    std::this_thread::sleep_for(kMachineLoadWindow);
    return AmbientBusyPercent(before, SampleSystemCpu()) / 100.0;
#else
    double average = 0.0;
    if (getloadavg(&average, 1) != 1)
    {
        return 0.0;
    }
    return average / static_cast<double>(std::max(1u, std::thread::hardware_concurrency()));
#endif
}
} // namespace GameEngine::Tests
