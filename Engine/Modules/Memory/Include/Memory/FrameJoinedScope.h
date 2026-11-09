#pragma once

#include <cstdint>

namespace GameEngine::Memory::Detail
{

#if GE_DEBUG_INSTRUMENTATION
/// Whether the frame joins the code on the calling thread: the frame thread in its frame, and a Run or ParallelFor
/// unit whose publisher is joined and waits it before the frame ends. Submit, EnqueueWork and channel bodies read
/// false, so work polled across frames goes through them. Thread-local; Debug and DebugFast only.
[[nodiscard]] bool IsFrameJoined() noexcept;

/// The one way to change the calling thread's frame-joined bit: sets it for the scope's
/// lifetime and restores the enclosing value when the scope ends, by unwinding too, so a
/// body that throws never leaves its bit on the thread for the bodies it runs next.
/// The job pool holds one around every body it runs; construct and destroy it on one
/// thread, in nested (LIFO) order.
class FrameJoinedScope
{
  public:
    explicit FrameJoinedScope(bool frameJoined) noexcept;
    ~FrameJoinedScope();

    FrameJoinedScope(const FrameJoinedScope&) = delete;
    FrameJoinedScope& operator=(const FrameJoinedScope&) = delete;

  private:
    bool m_Enclosing;
};

/// Count the Run units that frame-joined code published and that have not run yet. The job
/// pool adds one at the publish and removes it before the unit's counter decrement, so a
/// waiter never sees a stale count. The frame advance asserts that the count is zero.
void AddOutstandingJoinedRun() noexcept;
void RemoveOutstandingJoinedRun() noexcept;
[[nodiscard]] std::uint32_t OutstandingJoinedRuns() noexcept;
#endif

} // namespace GameEngine::Memory::Detail
