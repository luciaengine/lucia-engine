#pragma once
#include <cstddef>

namespace MeshRetirementAllocationFault
{
// Thread-local and enabled only around the operation under test. The executable
// owns the production sources so their STL allocations use the same override.
void Arm(std::size_t bytes, unsigned matchingAllocation);
void Disarm();
bool Triggered();
} // namespace MeshRetirementAllocationFault
