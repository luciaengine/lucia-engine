// Process-wide allocation counter for the slice-6 Submit-path microbench
// (TaskSlabPoolTests.cpp). Replaces the global allocation functions with
// counting forwarders to malloc/free — behavior is unchanged, every
// operator-new call in exe-compiled code bumps one relaxed atomic.
//
// Scope note (MSVC /MD): replacement intercepts allocations from code
// compiled INTO this executable (JobSystem, Engine static libs, the tests).
// Allocations inside foreign DLLs (CoreCLR, driver DLLs) bind to their own
// CRT and are not counted — which is exactly the scope the microbench wants.

#include <atomic>
#include <cstdlib>
#include <new>

#if defined(_MSC_VER)
#include <malloc.h>
#endif

namespace JobSystemTestHooks
{
std::atomic<unsigned long long> g_GlobalNewCount{0};

unsigned long long GetGlobalNewCount()
{
    return g_GlobalNewCount.load(std::memory_order_relaxed);
}
} // namespace JobSystemTestHooks

namespace
{

void* CountedAlloc(std::size_t size)
{
    JobSystemTestHooks::g_GlobalNewCount.fetch_add(1, std::memory_order_relaxed);
    return std::malloc(size != 0 ? size : 1);
}

void* CountedAlignedAlloc(std::size_t size, std::size_t alignment)
{
    JobSystemTestHooks::g_GlobalNewCount.fetch_add(1, std::memory_order_relaxed);
#if defined(_MSC_VER)
    return _aligned_malloc(size != 0 ? size : 1, alignment);
#else
    if (size == 0)
        size = alignment;
    return std::aligned_alloc(alignment, (size + alignment - 1) / alignment * alignment);
#endif
}

void AlignedFree(void* p) noexcept
{
#if defined(_MSC_VER)
    _aligned_free(p);
#else
    std::free(p);
#endif
}

} // namespace

// ---- unaligned forms ------------------------------------------------------

void* operator new(std::size_t size)
{
    if (void* p = CountedAlloc(size))
        return p;
    throw std::bad_alloc{};
}

void* operator new[](std::size_t size)
{
    if (void* p = CountedAlloc(size))
        return p;
    throw std::bad_alloc{};
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept { return CountedAlloc(size); }
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept { return CountedAlloc(size); }

void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }

// ---- aligned forms --------------------------------------------------------

void* operator new(std::size_t size, std::align_val_t alignment)
{
    if (void* p = CountedAlignedAlloc(size, static_cast<std::size_t>(alignment)))
        return p;
    throw std::bad_alloc{};
}

void* operator new[](std::size_t size, std::align_val_t alignment)
{
    if (void* p = CountedAlignedAlloc(size, static_cast<std::size_t>(alignment)))
        return p;
    throw std::bad_alloc{};
}

void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
    return CountedAlignedAlloc(size, static_cast<std::size_t>(alignment));
}

void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
    return CountedAlignedAlloc(size, static_cast<std::size_t>(alignment));
}

void operator delete(void* p, std::align_val_t) noexcept { AlignedFree(p); }
void operator delete[](void* p, std::align_val_t) noexcept { AlignedFree(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { AlignedFree(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { AlignedFree(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { AlignedFree(p); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { AlignedFree(p); }
