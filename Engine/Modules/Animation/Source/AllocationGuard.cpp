#include "Animation/AllocationGuard.h"

namespace GameEngine
{
namespace Animation
{

// Thread-local allocation counter. Bumped by the project's operator new
// hook in Engine/Source/Memory/MimallocOverrides.cpp under GE_DEV_DIAG
// (Debug + DebugFast + RelWithDebInfo). Release leaves it at 0.
thread_local std::atomic<std::size_t> g_ThreadAllocations{0};

AllocationGuard::AllocationGuard()
    : m_StartCount(g_ThreadAllocations.load(std::memory_order_relaxed))
{
}

std::size_t AllocationGuard::Count() const
{
    const std::size_t now = g_ThreadAllocations.load(std::memory_order_relaxed);
    return now - m_StartCount;
}

} // namespace Animation
} // namespace GameEngine
