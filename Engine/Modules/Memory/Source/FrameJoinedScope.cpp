// The calling thread's frame-joined bit behind Detail::FrameJoinedScope, one per thread for the whole process: Engine
// owns it, and the plain functions (no GE_API) export through exports.def on Windows and default visibility on POSIX.
// A Run closure built in a game module therefore reads and writes Engine's bit, never a copy of its own. The
// process-wide count of joined Run units not yet run lives here too, for the frame advance's check.

#include "Memory/FrameJoinedScope.h"

#include <atomic>
#include <cstdint>
#include <utility>

namespace GameEngine::Memory::Detail
{

#if GE_DEBUG_INSTRUMENTATION
namespace
{
/// Trivially destructible, so code that runs during static destruction or thread exit
/// still reads a valid value; it needs no exit sentinel.
constinit thread_local bool t_FrameJoined = false;

/// The joined Run units published and not yet run, over every thread.
constinit std::atomic<std::uint32_t> s_OutstandingJoinedRuns{0};
} // namespace

bool IsFrameJoined() noexcept
{
    return t_FrameJoined;
}

FrameJoinedScope::FrameJoinedScope(bool frameJoined) noexcept
    : m_Enclosing(std::exchange(t_FrameJoined, frameJoined))
{
}

FrameJoinedScope::~FrameJoinedScope()
{
    t_FrameJoined = m_Enclosing;
}

void AddOutstandingJoinedRun() noexcept
{
    s_OutstandingJoinedRuns.fetch_add(1, std::memory_order_relaxed);
}

// The pool calls it before the unit's counter decrement, so the waiter that the decrement releases reads the new count.
void RemoveOutstandingJoinedRun() noexcept
{
    s_OutstandingJoinedRuns.fetch_sub(1, std::memory_order_relaxed);
}

std::uint32_t OutstandingJoinedRuns() noexcept
{
    return s_OutstandingJoinedRuns.load(std::memory_order_relaxed);
}
#endif

} // namespace GameEngine::Memory::Detail
