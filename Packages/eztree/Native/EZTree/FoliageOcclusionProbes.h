#pragma once

#include <cstddef>

namespace GameEngine
{

struct Mesh;

namespace EZTree
{

class FoliageOcclusionProbes
{
public:
    // Sample the rest-pose canopy once per leaf and write the occlusion to UV1.y.
    static void Bake(Mesh& leaves, std::size_t verticesPerLeaf, float leafSize);
};

} // namespace EZTree
} // namespace GameEngine
