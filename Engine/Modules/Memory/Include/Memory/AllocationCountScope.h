#pragma once

#include <cstdint>

namespace GameEngine::Memory
{

/// Which allocations an `AllocationCountScope` counts.
enum class CountWindow : std::uint8_t
{
    /// Allocations made on the thread that constructed the scope.
    ThisThread,
    /// Allocations made on every thread while the scope is open: the window for a path
    /// that forks units onto workers, whose allocations a `ThisThread` window misses.
    Process,
};

/// Counts global `operator new` calls (every form) from construction until `Count()`.
///
/// Only images built with Memory/AllocationHook.inl report allocations. The system-allocator
/// test build instruments Engine in Debug/DebugFast and the allocation-counter and world-draw
/// test executables. Other images and direct malloc/aligned_alloc calls are not counted.
/// Release Engine builds and mimalloc builds do not carry this hook.
///
/// Counts are a measurement for tests and labs, never timing. Overlapping scopes each
/// read their own delta.
///
///     Memory::AllocationCountScope scope(Memory::CountWindow::Process);
///     RunTheFrame();
///     EXPECT_EQ(scope.Count(), 0u);
class AllocationCountScope
{
  public:
    /// Opens the window. No default: every caller states the reach it measures.
    explicit AllocationCountScope(CountWindow window);
    ~AllocationCountScope();

    AllocationCountScope(const AllocationCountScope&) = delete;
    AllocationCountScope& operator=(const AllocationCountScope&) = delete;
    AllocationCountScope(AllocationCountScope&&) = delete;
    AllocationCountScope& operator=(AllocationCountScope&&) = delete;

    /// Allocations counted in this window since construction. A `ThisThread` scope is
    /// read on the thread that constructed it (asserted in Debug).
    std::uint64_t Count() const;

  private:
    CountWindow m_Window;
    std::uint64_t m_Start = 0;
    /// The constructing thread's counter block, for the `ThisThread` read check.
    [[maybe_unused]] const void* m_Thread = nullptr;
};

} // namespace GameEngine::Memory
