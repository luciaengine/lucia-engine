#pragma once

#include <atomic>
#include <cstddef>

namespace GameEngine
{
namespace Animation
{

// RAII allocation counter for per-thread allocation tracking in tests.
//
// Wraps a thread-local counter that test code can read to assert
// zero allocations after warm-up in a hot path:
//
//   void DoWork();
//
//   TEST(IK, ZeroAllocsAfterWarmup) {
//       // Warm up
//       DoWork();
//       // Measure
//       AllocationGuard guard;
//       DoWork();
//       EXPECT_EQ(guard.Count(), 0u);
//   }
//
// The project's operator new hook in Engine/Source/Memory/MimallocOverrides.cpp
// bumps g_ThreadAllocations on each allocation when GE_DEV_DIAG is defined
// (Debug + DebugFast + RelWithDebInfo). Release leaves the counter at 0,
// so guard.Count() always returns 0 there. CI exercises the guarded
// assertions in Debug / DebugFast.
class AllocationGuard
{
public:
    AllocationGuard();
    ~AllocationGuard() = default;

    AllocationGuard(const AllocationGuard&) = delete;
    AllocationGuard& operator=(const AllocationGuard&) = delete;

    // Number of allocations on the current thread since this guard was
    // constructed.
    std::size_t Count() const;

private:
    std::size_t m_StartCount = 0;
};

// Thread-local counter (defined in AllocationGuard.cpp).
// Bumped by the project's operator new override on hot-path-relevant
// builds; left at 0 in release.
extern thread_local std::atomic<std::size_t> g_ThreadAllocations;

} // namespace Animation
} // namespace GameEngine
