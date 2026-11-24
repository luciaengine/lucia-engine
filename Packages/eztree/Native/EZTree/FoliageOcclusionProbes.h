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
    // Bake rest-pose occlusion into UV1.y of a generated mesh with computed bounds and complete UV1.
    // Each leaf occupies four or eight consecutive vertices, with a square card first.
    static void Bake(Mesh& leaves, std::size_t verticesPerLeaf, float leafSize);
};

} // namespace EZTree
} // namespace GameEngine
