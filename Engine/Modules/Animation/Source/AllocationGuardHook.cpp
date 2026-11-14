// AllocationGuardHook.cpp
//
// Provides a project-wide `operator new` hook that increments
// `Animation::g_ThreadAllocations` on every allocation. This is the
// signal AllocationGuard reads to verify that hot paths are
// allocation-free after warm-up.
//
// Linkage rules:
//   - DO NOT add this file to the Animation static library.
//   - Editor + GameEngine.Native pull the same hook through
//     Engine/Source/Memory/MimallocOverrides.cpp (which forwards to
//     mimalloc and increments the same Animation counter).
//   - Tests that need the AllocationGuard signal must link this file
//     directly via target_sources(); since tests don't link mimalloc
//     overrides this is the only translation unit that bumps the
//     counter for the test process.
//
// Both translation units cooperate cleanly because they target
// distinct executables (the test exe has only this; Editor has only
// the mimalloc one).

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <new>

#if defined(GE_DEV_DIAG)
namespace GameEngine { namespace Animation {
extern thread_local std::atomic<std::size_t> g_ThreadAllocations;
}}
namespace
{
inline void GE_NoteAllocation()
{
	GameEngine::Animation::g_ThreadAllocations.fetch_add(1, std::memory_order_relaxed);
}
}
#else
namespace { inline void GE_NoteAllocation() {} }
#endif

#if defined(_MSC_VER)
	#pragma warning(push)
	#pragma warning(disable : 28251)
#endif

void* operator new(std::size_t n)
{
	GE_NoteAllocation();
	void* p = std::malloc(n ? n : 1);
	if (!p) throw std::bad_alloc();
	return p;
}

void* operator new[](std::size_t n)
{
	GE_NoteAllocation();
	void* p = std::malloc(n ? n : 1);
	if (!p) throw std::bad_alloc();
	return p;
}

void* operator new(std::size_t n, const std::nothrow_t&) noexcept
{
	GE_NoteAllocation();
	return std::malloc(n ? n : 1);
}

void* operator new[](std::size_t n, const std::nothrow_t&) noexcept
{
	GE_NoteAllocation();
	return std::malloc(n ? n : 1);
}

void operator delete(void* p) noexcept
{
	std::free(p);
}

void operator delete[](void* p) noexcept
{
	std::free(p);
}

void operator delete(void* p, std::size_t /*n*/) noexcept
{
	std::free(p);
}

void operator delete[](void* p, std::size_t /*n*/) noexcept
{
	std::free(p);
}

void operator delete(void* p, const std::nothrow_t&) noexcept
{
	std::free(p);
}

void operator delete[](void* p, const std::nothrow_t&) noexcept
{
	std::free(p);
}

#if defined(_MSC_VER)
	#pragma warning(pop)
#endif
