#include "MeshRetirementAllocationFault.h"
#include <cstdlib>
#include <new>

namespace
{
thread_local std::size_t Bytes = 0;
thread_local unsigned Remaining = 0;
thread_local bool Fired = false;
void* Allocate(std::size_t bytes)
{
    if (Remaining && (!Bytes || Bytes == bytes) && --Remaining == 0)
    {
        Fired = true;
        throw std::bad_alloc{};
    }
    if (void* p = std::malloc(bytes ? bytes : 1))
        return p;
    throw std::bad_alloc{};
}
} // namespace
void MeshRetirementAllocationFault::Arm(std::size_t bytes, unsigned matchingAllocation)
{
    Bytes = bytes;
    Remaining = matchingAllocation;
    Fired = false;
}
void MeshRetirementAllocationFault::Disarm()
{
    Remaining = 0;
}
bool MeshRetirementAllocationFault::Triggered()
{
    return Fired;
}
void* operator new(std::size_t bytes)
{
    return Allocate(bytes);
}
void* operator new[](std::size_t bytes)
{
    return Allocate(bytes);
}
void operator delete(void* p) noexcept
{
    std::free(p);
}
void operator delete[](void* p) noexcept
{
    std::free(p);
}
void operator delete(void* p, std::size_t) noexcept
{
    std::free(p);
}
void operator delete[](void* p, std::size_t) noexcept
{
    std::free(p);
}
