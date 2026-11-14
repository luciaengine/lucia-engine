#pragma once

#include <cstddef>
#include <limits>
#include <memory>
#include <memory_resource>
#include <new>
#include <span>
#include <type_traits>

namespace GameEngine::Memory
{

/// Memory valid through the end of the next frame, for the frame thread in its frame and the `Run`
/// and `ParallelFor` units it joins. Debug and DebugFast refuse a `Submit` or `EnqueueWork` body.
/// Each thread allocates from its own two arenas, and nothing is freed one by one. The rules are in
/// docs/scripting/memory-allocators.mdx.
class FrameMemory
{
  public:
    FrameMemory() = delete;

    /// `alignment` is a power of two.
    static void* Allocate(std::size_t bytes, std::size_t alignment = alignof(std::max_align_t));

    /// `count` value-initialised elements (zeroes for arithmetic types).
    template <class T>
    static std::span<T> AllocateArray(std::size_t count);

    /// `count` elements left indeterminate, for arrays the caller fills before reading.
    template <class T>
    static std::span<T> AllocateArrayForOverwrite(std::size_t count);

    /// A copy of `source` in frame memory.
    template <class T>
    static std::span<T> CopyArray(std::span<const T> source);

    /// One memory resource for every thread: each allocation comes from the calling thread's
    /// frame memory. Deallocation does nothing.
    static std::pmr::memory_resource& Resource();

  private:
    static void* AllocateArrayBytes(std::size_t count, std::size_t elementBytes, std::size_t alignment);
};

namespace Detail
{
/// Starts the next frame: frame memory from two frames ago becomes free for reuse. The frame
/// loop calls it on the frame thread at the start of each frame, before any frame code runs.
/// Debug and DebugFast assert that every `Run` unit the last frame published has run.
void AdvanceFrameIndex();

#if GE_DEBUG_INSTRUMENTATION
/// Tells the thread-exit check that no frame code runs until the next `AdvanceFrameIndex`, so
/// a thread may exit with frame memory from the last frames. The frame loop calls it when it ends.
void StopFrameLoop() noexcept;
#endif
} // namespace Detail

inline void* FrameMemory::AllocateArrayBytes(std::size_t count, std::size_t elementBytes, std::size_t alignment)
{
    if (elementBytes != 0 && count > std::numeric_limits<std::size_t>::max() / elementBytes)
        throw std::bad_array_new_length();
    return Allocate(count * elementBytes, alignment);
}

template <class T>
std::span<T> FrameMemory::AllocateArray(std::size_t count)
{
    static_assert(std::is_trivially_destructible_v<T>,
                  "FrameMemory arrays hold trivially destructible types; use a std::pmr container on Resource()");
    T* elements = static_cast<T*>(AllocateArrayBytes(count, sizeof(T), alignof(T)));
    std::uninitialized_value_construct_n(elements, count);
    return {elements, count};
}

template <class T>
std::span<T> FrameMemory::AllocateArrayForOverwrite(std::size_t count)
{
    static_assert(std::is_trivially_destructible_v<T>,
                  "FrameMemory arrays hold trivially destructible types; use a std::pmr container on Resource()");
    T* elements = static_cast<T*>(AllocateArrayBytes(count, sizeof(T), alignof(T)));
    std::uninitialized_default_construct_n(elements, count);
    return {elements, count};
}

template <class T>
std::span<T> FrameMemory::CopyArray(std::span<const T> source)
{
    static_assert(std::is_trivially_destructible_v<T>,
                  "FrameMemory arrays hold trivially destructible types; use a std::pmr container on Resource()");
    T* elements = static_cast<T*>(AllocateArrayBytes(source.size(), sizeof(T), alignof(T)));
    std::uninitialized_copy(source.begin(), source.end(), elements);
    return {elements, source.size()};
}

} // namespace GameEngine::Memory
