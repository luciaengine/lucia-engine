// The allocation counter's hook: replacements for every global operator new and
// operator delete form. Each allocation is reported to Memory::Detail::OnAllocation
// and then served by the C runtime's malloc (aligned forms: _aligned_malloc on
// Windows, posix_memalign elsewhere); each deallocation goes straight back to the
// matching free. Behaviour is the default library's, plus the count.
//
// Include in exactly one translation unit per explicitly instrumented image. The
// system-allocator test build attaches it to Engine in Debug/DebugFast and to
// MemoryCounterTests and EngineWorldDrawTests. Each hook calls the one counter
// inside Engine, so allocations in the shared library and executable share a window.
// Do not combine it with mimalloc or another new/delete replacement in one image.

#include "Memory/AllocationCounter.h"

#include <cstddef>
#include <cstdlib>
#include <new>

#if defined(_MSC_VER)
#include <malloc.h>
// C28251: inconsistent annotation for the global allocation functions. The
// replacements carry no SAL annotations by design.
#pragma warning(push)
#pragma warning(disable : 28251)
#endif

namespace
{

void* AllocationHookRawAllocate(std::size_t bytes)
{
    return std::malloc(bytes != 0 ? bytes : 1);
}

void* AllocationHookRawAllocateAligned(std::size_t bytes, std::size_t alignment)
{
#if defined(_MSC_VER)
    return _aligned_malloc(bytes != 0 ? bytes : 1, alignment);
#else
    void* memory = nullptr;
    const std::size_t posixAlignment = alignment < sizeof(void*) ? sizeof(void*) : alignment;
    return posix_memalign(&memory, posixAlignment, bytes != 0 ? bytes : 1) == 0 ? memory : nullptr;
#endif
}

void AllocationHookRawFreeAligned(void* memory) noexcept
{
#if defined(_MSC_VER)
    _aligned_free(memory);
#else
    std::free(memory);
#endif
}

/// The throwing allocation path; `alignment` 0 is the plain forms' malloc. A denied
/// allocation throws at once; a failed one runs the installed new-handler and retries,
/// as the default operator new does.
void* AllocationHookAllocate(std::size_t bytes, std::size_t alignment)
{
    if (!GameEngine::Memory::Detail::OnAllocation(bytes))
        throw std::bad_alloc();
    for (;;)
    {
        void* memory =
            alignment != 0 ? AllocationHookRawAllocateAligned(bytes, alignment) : AllocationHookRawAllocate(bytes);
        if (memory != nullptr)
            return memory;
        const std::new_handler handler = std::get_new_handler();
        if (handler == nullptr)
            throw std::bad_alloc();
        handler();
    }
}

void* AllocationHookAllocateNoThrow(std::size_t bytes, std::size_t alignment) noexcept
{
    try
    {
        return AllocationHookAllocate(bytes, alignment);
    }
    catch (...)
    {
        return nullptr;
    }
}

} // namespace

void* operator new(std::size_t bytes)
{
    return AllocationHookAllocate(bytes, 0);
}

void* operator new[](std::size_t bytes)
{
    return AllocationHookAllocate(bytes, 0);
}

void* operator new(std::size_t bytes, const std::nothrow_t&) noexcept
{
    return AllocationHookAllocateNoThrow(bytes, 0);
}

void* operator new[](std::size_t bytes, const std::nothrow_t&) noexcept
{
    return AllocationHookAllocateNoThrow(bytes, 0);
}

void* operator new(std::size_t bytes, std::align_val_t alignment)
{
    return AllocationHookAllocate(bytes, static_cast<std::size_t>(alignment));
}

void* operator new[](std::size_t bytes, std::align_val_t alignment)
{
    return AllocationHookAllocate(bytes, static_cast<std::size_t>(alignment));
}

void* operator new(std::size_t bytes, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
    return AllocationHookAllocateNoThrow(bytes, static_cast<std::size_t>(alignment));
}

void* operator new[](std::size_t bytes, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
    return AllocationHookAllocateNoThrow(bytes, static_cast<std::size_t>(alignment));
}

void operator delete(void* memory) noexcept
{
    std::free(memory);
}

void operator delete[](void* memory) noexcept
{
    std::free(memory);
}

void operator delete(void* memory, const std::nothrow_t&) noexcept
{
    std::free(memory);
}

void operator delete[](void* memory, const std::nothrow_t&) noexcept
{
    std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept
{
    std::free(memory);
}

void operator delete[](void* memory, std::size_t) noexcept
{
    std::free(memory);
}

void operator delete(void* memory, std::align_val_t) noexcept
{
    AllocationHookRawFreeAligned(memory);
}

void operator delete[](void* memory, std::align_val_t) noexcept
{
    AllocationHookRawFreeAligned(memory);
}

void operator delete(void* memory, std::size_t, std::align_val_t) noexcept
{
    AllocationHookRawFreeAligned(memory);
}

void operator delete[](void* memory, std::size_t, std::align_val_t) noexcept
{
    AllocationHookRawFreeAligned(memory);
}

void operator delete(void* memory, std::align_val_t, const std::nothrow_t&) noexcept
{
    AllocationHookRawFreeAligned(memory);
}

void operator delete[](void* memory, std::align_val_t, const std::nothrow_t&) noexcept
{
    AllocationHookRawFreeAligned(memory);
}

#if defined(_MSC_VER)
#pragma warning(pop)
#endif
