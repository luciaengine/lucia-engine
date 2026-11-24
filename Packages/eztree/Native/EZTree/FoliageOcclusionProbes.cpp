#include "EZTree/FoliageOcclusionProbes.h"

#include "Assets/ModelAsset.h"
#include "Mathematics/Interpolation.h"
#include "Mathematics/Vector3.h"
#include "Mathematics/Vector3Int.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace GameEngine::EZTree
{
namespace
{

using Mathematics::Vector3;
using Mathematics::Vector3Int;

constexpr int kCellsPerAxis = 8;
constexpr int kProbesPerAxis = kCellsPerAxis + 1;

std::size_t GridIndex(Vector3Int point, int stride)
{
    return (static_cast<std::size_t>(point.Z) * stride + point.Y) * stride + point.X;
}

bool IsInsideGrid(Vector3Int cell)
{
    return cell.X >= 0 && cell.X < kCellsPerAxis &&
           cell.Y >= 0 && cell.Y < kCellsPerAxis &&
           cell.Z >= 0 && cell.Z < kCellsPerAxis;
}

Vector3Int CellAt(Vector3 point)
{
    constexpr float kLastCell = static_cast<float>(kCellsPerAxis - 1);
    return {
        static_cast<int>(std::clamp(point.x, 0.0f, kLastCell)),
        static_cast<int>(std::clamp(point.y, 0.0f, kLastCell)),
        static_cast<int>(std::clamp(point.z, 0.0f, kLastCell))
    };
}

Vector3 ReadPosition(const Vertex& vertex)
{
    return {vertex.Position[0], vertex.Position[1], vertex.Position[2]};
}

Vector3 LeafCenterInGrid(const Mesh& leaves, std::size_t first, Vector3 origin, float inverseCellWidth)
{
    // Opposite corners of the first card give the center shared by both crossed cards.
    const Vector3 center = (ReadPosition(leaves.Vertices[first]) + ReadPosition(leaves.Vertices[first + 2])) * 0.5f;
    return (center - origin) * inverseCellWidth;
}

} // namespace

void FoliageOcclusionProbes::Bake(Mesh& leaves, std::size_t verticesPerLeaf, float leafSize)
{
    if (leaves.Vertices.size() <= verticesPerLeaf)
        return;

    const Vector3 minimum{leaves.MinBounds[0], leaves.MinBounds[1], leaves.MinBounds[2]};
    const Vector3 extent = Vector3{leaves.MaxBounds[0], leaves.MaxBounds[1], leaves.MaxBounds[2]} - minimum;
    // Use cubic cells for equal ray lengths and keep the cells wider than a typical leaf.
    const float cellWidth = std::max(leafSize * 2.0f, std::max({extent.x, extent.y, extent.z}) / kCellsPerAxis);
    const float inverseCellWidth = 1.0f / cellWidth;

    std::array<float, kCellsPerAxis * kCellsPerAxis * kCellsPerAxis> opticalDepth{};
    // Estimate the projected opaque area of a cutout leaf from its first card. A
    // crossed billboard still represents one leaf, and contributes only once.
    constexpr float kProjectedCutoutCoverage = 0.25f;
    const float areaScale = kProjectedCutoutCoverage * inverseCellWidth * inverseCellWidth;
    for (std::size_t first = 0; first < leaves.Vertices.size(); first += verticesPerLeaf)
    {
        const Vector3 center = LeafCenterInGrid(leaves, first, minimum, inverseCellWidth);
        const float cardArea = (ReadPosition(leaves.Vertices[first + 2]) -
                                ReadPosition(leaves.Vertices[first + 1])).LengthSquared();
        if (!std::isfinite(center.x) || !std::isfinite(center.y) || !std::isfinite(center.z) || !std::isfinite(cardArea))
            return;
        opticalDepth[GridIndex(CellAt(center), kCellsPerAxis)] += cardArea * areaScale;
    }

    std::array<float, kProbesPerAxis * kProbesPerAxis * kProbesPerAxis> occlusion{};
    constexpr float kMaximumOcclusion = 0.65f;
    constexpr float kOctantWeight = 1.0f / 8.0f;
    for (int z = 0; z < kProbesPerAxis; ++z)
    for (int y = 0; y < kProbesPerAxis; ++y)
    for (int x = 0; x < kProbesPerAxis; ++x)
    {
        const Vector3Int probe{x, y, z};
        float visibility = 0.0f;
        // Each probe sits at a grid corner. Integrate one diagonal ray per octant.
        for (int dz = 0; dz < 2; ++dz)
        for (int dy = 0; dy < 2; ++dy)
        for (int dx = 0; dx < 2; ++dx)
        {
            const Vector3Int step{2 * dx - 1, 2 * dy - 1, 2 * dz - 1};
            float depth = 0.0f;
            for (Vector3Int cell = probe + Vector3Int{dx - 1, dy - 1, dz - 1};
                 IsInsideGrid(cell); cell = cell + step)
            {
                depth += opticalDepth[GridIndex(cell, kCellsPerAxis)];
            }
            visibility += std::exp(-depth);
        }
        occlusion[GridIndex(probe, kProbesPerAxis)] = kMaximumOcclusion * (1.0f - visibility * kOctantWeight);
    }

    for (std::size_t first = 0; first < leaves.Vertices.size(); first += verticesPerLeaf)
    {
        const Vector3 point = LeafCenterInGrid(leaves, first, minimum, inverseCellWidth);
        const Vector3Int cell = CellAt(point);
        const Vector3 offset = point - static_cast<Vector3>(cell);
        const Vector3 fraction{Math::Clamp01(offset.x), Math::Clamp01(offset.y), Math::Clamp01(offset.z)};
        const Vector3 lowerWeight = Vector3{1.0f, 1.0f, 1.0f} - fraction;
        float leafOcclusion = 0.0f;
        for (int z = 0; z < 2; ++z)
        for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 2; ++x)
        {
            const float weight = (x ? fraction.x : lowerWeight.x) *
                                 (y ? fraction.y : lowerWeight.y) *
                                 (z ? fraction.z : lowerWeight.z);
            leafOcclusion += occlusion[GridIndex(cell + Vector3Int{x, y, z}, kProbesPerAxis)] * weight;
        }
        for (std::size_t vertex = first; vertex < first + verticesPerLeaf; ++vertex)
            leaves.TexCoords1[2 * vertex + 1] = leafOcclusion;
    }
}

} // namespace GameEngine::EZTree
