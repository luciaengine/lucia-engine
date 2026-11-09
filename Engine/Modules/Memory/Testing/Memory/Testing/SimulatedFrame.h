#pragma once

#include "Memory/FrameJoinedScope.h"
#include "Memory/FrameMemory.h"

namespace GameEngine::Memory::Testing
{

/// One frame of a frame loop, for a test: it starts the next frame and sets the frame-joined bit
/// on the calling thread, so the test thread and the units it forks with `Run` or `ParallelFor`
/// may use `FrameMemory`, as in `Application::Tick`.
///
///     {
///         Memory::Testing::SimulatedFrame frame;
///         std::span<int> values = Memory::FrameMemory::AllocateArray<int>(64);
///         ...                                  // valid through the end of the next frame
///     }
///
/// Its end restores the bit and stops the loop, so a pool shut down after the test's last frame
/// frees its threads' frame memory without the thread-exit check. Construct and destroy it on
/// one thread, never nested.
class SimulatedFrame
{
  public:
    SimulatedFrame() { Detail::AdvanceFrameIndex(); }
    ~SimulatedFrame()
    {
#if GE_DEBUG_INSTRUMENTATION
        Detail::StopFrameLoop();
#endif
    }

    SimulatedFrame(const SimulatedFrame&) = delete;
    SimulatedFrame& operator=(const SimulatedFrame&) = delete;

  private:
#if GE_DEBUG_INSTRUMENTATION
    Detail::FrameJoinedScope m_FrameJoined{true};
#endif
};

} // namespace GameEngine::Memory::Testing
