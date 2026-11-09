// FrameMemory's frame index and thread state. Each thread owns two arenas, one per frame parity, and an
// allocation in an arena that holds an older frame resets it first, on its own thread. The state is one
// trivially destructible thread_local, freed by an exit sentinel as scratch's is. No function here is a
// template, so Engine.dll's generated export table carries every entry point.

#include "Memory/FrameMemory.h"

#include "Memory/Arena.h"
#include "Memory/FrameJoinedScope.h"
#include "Memory/StatsKind.h"
#include "RetentionWindow.h"
#include "Types/FalseSharing.h"

#include <atomic>
#include <cassert>
#include <cstdint>
#include <string_view>
#include <utility>

namespace GameEngine::Memory
{
namespace Detail
{
namespace
{

/// The frame the frame loop is in, 0 before its first advance. Only the frame thread writes it.
constinit std::atomic<std::uint64_t> s_FrameIndex{0};
#if GE_DEBUG_INSTRUMENTATION
/// True from an advance to `StopFrameLoop`: while it is, no thread may exit with live frame memory.
constinit std::atomic<bool> s_FrameLoopRunning{false};
#endif

} // namespace

/// One of a thread's two frame arenas, with the frame whose memory it holds.
class FrameArena
{
  public:
    explicit FrameArena(std::string_view statsName) : m_Arena(ThreadArenaTag{}, statsName, StatsKind::FrameMemory) {}

    FrameArena(const FrameArena&) = delete;
    FrameArena& operator=(const FrameArena&) = delete;

    /// Allocates for `frame`, first resetting the arena when it holds an earlier frame.
    void* Allocate(std::size_t bytes, std::size_t alignment, std::uint64_t frame)
    {
        if (m_Frame != frame) [[unlikely]]
            Rewind(frame);
        return m_Arena.Allocate(bytes, alignment);
    }

    /// Whether it holds memory of `frame` or of the frame before it.
    bool HoldsLiveMemory(std::uint64_t frame) const { return m_Arena.BytesUsed() != 0 && m_Frame + 1 >= frame; }

  private:
    /// A reset that publishes the stats, then the retention rule over the last 1024 resets.
    void Rewind(std::uint64_t frame)
    {
        m_Retention.Take(m_Arena.PeakBytesSinceReset());
        m_Arena.Reset();
        m_Arena.TrimRetained(m_Retention.Advance(1));
        m_Frame = frame;
    }

    Arena m_Arena;
    std::uint64_t m_Frame = 0;
    RetentionWindow m_Retention;
};

/// One thread's frame memory, the arena of even frames and the arena of odd frames, created at
/// its first frame allocation and freed by its exit sentinel. A full separation before and after
/// its state keeps the bump cursors off every line a neighbouring allocation uses.
class FrameThread
{
  public:
    void* Allocate(std::size_t bytes, std::size_t alignment, std::uint64_t frame)
    {
        return m_Arenas[frame & 1].Allocate(bytes, alignment, frame);
    }

    bool HoldsLiveMemory(std::uint64_t frame) const
    {
        return m_Arenas[0].HoldsLiveMemory(frame) || m_Arenas[1].HoldsLiveMemory(frame);
    }

  private:
    [[maybe_unused]] char m_LeadingSeparation[kFalseSharingSeparation];
    FrameArena m_Arenas[2]{FrameArena{"Memory.Frame.Even"}, FrameArena{"Memory.Frame.Odd"}};
    [[maybe_unused]] char m_TrailingSeparation[kFalseSharingSeparation];
};

namespace
{

enum class FrameThreadState : std::uint8_t
{
    /// The exit sentinel has not run. A null pointer means the thread has not allocated frame
    /// memory; a non-null one, that its state exists and the sentinel is registered.
    Unregistered,
    /// Its exit sentinel has run.
    Exited,
};

struct FrameThreadSlot
{
    FrameThread* Thread = nullptr;
    FrameThreadState State = FrameThreadState::Unregistered;
};

constinit thread_local FrameThreadSlot t_Frame;

/// Frees the thread's state at thread exit and marks it Exited. Registered with the thread by
/// its first frame allocation, which names it.
struct FrameExitSentinel
{
    ~FrameExitSentinel()
    {
        t_Frame.State = FrameThreadState::Exited;
        FrameThread* thread = std::exchange(t_Frame.Thread, nullptr);
#if GE_DEBUG_INSTRUMENTATION
        assert((thread == nullptr || !s_FrameLoopRunning.load(std::memory_order_relaxed) ||
                !thread->HoldsLiveMemory(s_FrameIndex.load(std::memory_order_relaxed))) &&
               "a thread exited holding frame memory of this frame or the last one while the frame loop runs; "
               "frame memory is only for the frame thread and the forks it joins");
#endif
        delete thread;
    }
};

thread_local FrameExitSentinel t_FrameExit;

/// The calling thread's state, created at its first frame allocation; null after its exit
/// sentinel has run.
FrameThread* AcquireFrameThread()
{
    FrameThreadSlot& slot = t_Frame;
    if (slot.Thread != nullptr) [[likely]]
        return slot.Thread;
    if (slot.State == FrameThreadState::Exited)
        return nullptr;
    // Naming the sentinel registers its destructor with this thread.
    [[maybe_unused]] FrameExitSentinel& sentinel = t_FrameExit;
    slot.Thread = new FrameThread();
    return slot.Thread;
}

/// The one resource every frame container holds: it serves the thread that grows the container.
class FrameMemoryResource final : public std::pmr::memory_resource
{
  private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override
    {
        return FrameMemory::Allocate(bytes, alignment);
    }
    void do_deallocate(void*, std::size_t, std::size_t) override {}
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override { return this == &other; }
};

FrameMemoryResource s_Resource;

} // namespace

void AdvanceFrameIndex()
{
#if GE_DEBUG_INSTRUMENTATION
    assert(OutstandingJoinedRuns() == 0 &&
           "a Run unit that frame code published is still outstanding at the frame advance; wait its counter in "
           "the frame that publishes it, or use Submit or EnqueueWork for work that outlives the frame");
    s_FrameLoopRunning.store(true, std::memory_order_relaxed);
#endif
    s_FrameIndex.fetch_add(1, std::memory_order_relaxed);
}

#if GE_DEBUG_INSTRUMENTATION
void StopFrameLoop() noexcept
{
    s_FrameLoopRunning.store(false, std::memory_order_relaxed);
}
#endif

} // namespace Detail

void* FrameMemory::Allocate(std::size_t bytes, std::size_t alignment)
{
    assert(alignment != 0 && (alignment & (alignment - 1)) == 0 && "FrameMemory alignment must be a power of two");
    const std::uint64_t frame = Detail::s_FrameIndex.load(std::memory_order_relaxed);
#if GE_DEBUG_INSTRUMENTATION
    assert(frame != 0 && "frame memory needs a running frame loop; use a ScratchScope or an Arena you own");
    assert(Detail::IsFrameJoined() &&
           "frame memory is only for the frame thread and the forks it joins; this body may outlive the frame: "
           "use an Arena you own");
#endif
    Detail::FrameThread* thread = Detail::AcquireFrameThread();
    if (thread == nullptr) [[unlikely]]
    {
        // The thread is exiting: the block is never freed.
        return ::operator new(bytes, std::align_val_t{alignment});
    }
    return thread->Allocate(bytes, alignment, frame);
}

std::pmr::memory_resource& FrameMemory::Resource()
{
    return Detail::s_Resource;
}

} // namespace GameEngine::Memory
