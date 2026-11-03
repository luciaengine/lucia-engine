// MimallocOverrides.cpp
//
// Fallback global operator new/delete overrides used when the build does not provide
// mimalloc's official override target (e.g., mimalloc-override).
//
// IMPORTANT:
// - This translation unit must be linked into every module that participates in
//   cross-module allocation/free (Editor.exe and GameEngine.Native.dll).
// - Prefer linking mimalloc's official override library when available.

#include <mimalloc.h>

#include <atomic>
#include <cstddef>
#include <new>

// Animation::AllocationGuard hook. The Animation module declares a
// thread-local atomic counter (extern) that AllocationGuard reads as the
// "allocations since I was created" delta. Hot-path tests assert
// `guard.Count() == 0` after warm-up.
//
// Active in Debug + DebugFast + RelWithDebInfo (when GE_DEV_DIAG is defined).
// Compiled-out in Release. The hook is purposely thin: a single relaxed
// atomic increment per `operator new` / `operator new[]`.
//
// We pull in the Animation thread-local counter directly via an extern
// declaration. The Animation static library defines it; any module that
// links MimallocOverrides also already links Animation (Editor + Native
// DLL). Tests that need the hook link AllocationGuardHook.cpp directly
// (it embeds a thin `operator new` override of its own).
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
	// C28251: Inconsistent annotation for global operator new/delete overrides.
	// Our overrides intentionally omit SAL annotations; treat this as noise, not a bug.
	#pragma warning(push)
	#pragma warning(disable : 28251)
#endif

// ---- C++ new/delete overrides (module-wide) ----

void* operator new(std::size_t n)
{
	GE_NoteAllocation();
	return mi_new(n);
}

void* operator new[](std::size_t n)
{
	GE_NoteAllocation();
	return mi_new(n);
}

void* operator new(std::size_t n, const std::nothrow_t&) noexcept
{
	GE_NoteAllocation();
	return mi_new_nothrow(n);
}

void* operator new[](std::size_t n, const std::nothrow_t&) noexcept
{
	GE_NoteAllocation();
	return mi_new_nothrow(n);
}

void operator delete(void* p) noexcept
{
	mi_free(p);
}

void operator delete[](void* p) noexcept
{
	mi_free(p);
}

void operator delete(void* p, std::size_t /*n*/) noexcept
{
	// Ignore the size: it can be incorrect across module boundaries.
	mi_free(p);
}

void operator delete[](void* p, std::size_t /*n*/) noexcept
{
	mi_free(p);
}

void operator delete(void* p, const std::nothrow_t&) noexcept
{
	mi_free(p);
}

void operator delete[](void* p, const std::nothrow_t&) noexcept
{
	mi_free(p);
}

void* operator new(std::size_t n, std::align_val_t al)
{
	GE_NoteAllocation();
	return mi_new_aligned(n, static_cast<std::size_t>(al));
}

void* operator new[](std::size_t n, std::align_val_t al)
{
	GE_NoteAllocation();
	return mi_new_aligned(n, static_cast<std::size_t>(al));
}

void* operator new(std::size_t n, std::align_val_t al, const std::nothrow_t&) noexcept
{
	GE_NoteAllocation();
	return mi_new_aligned_nothrow(n, static_cast<std::size_t>(al));
}

void* operator new[](std::size_t n, std::align_val_t al, const std::nothrow_t&) noexcept
{
	GE_NoteAllocation();
	return mi_new_aligned_nothrow(n, static_cast<std::size_t>(al));
}

void operator delete(void* p, std::align_val_t al) noexcept
{
	(void)al;
	mi_free(p);
}

void operator delete[](void* p, std::align_val_t al) noexcept
{
	(void)al;
	mi_free(p);
}

void operator delete(void* p, std::size_t /*n*/, std::align_val_t al) noexcept
{
	(void)al;
	mi_free(p);
}

void operator delete[](void* p, std::size_t /*n*/, std::align_val_t al) noexcept
{
	(void)al;
	mi_free(p);
}

#if defined(_MSC_VER)
	#pragma warning(pop)
#endif
