// Engine allocation hook for system-allocator test builds. Release images stay uninstrumented.
#if GE_DEBUG_INSTRUMENTATION
#include "Memory/AllocationHook.inl"
#endif
