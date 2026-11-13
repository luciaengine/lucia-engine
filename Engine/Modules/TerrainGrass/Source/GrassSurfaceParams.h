#pragma once

#include "Mathematics/Interpolation.h"
#include "Mathematics/Rect.h"
#include "Types/Types.h"

#include <algorithm>
#include <span>

namespace GameEngine::TerrainGrass
{

// The terrains[] row no terrain uses: no row draws from pages this frame.
inline constexpr uint32 kGrassNoPagedTerrain = 0xFFFFFFFFu;

// std140 mirror of GrassSurfaceParamsBuffer in grass_surface_pages.glsl (placement binding 4, wide arm):
// the terrains[] row that draws from pages and the first word of its surface table in binding 5.
struct alignas(16) GrassSurfaceParamsGPU
{
    uint32 PagedTerrainRow = kGrassNoPagedTerrain;
    uint32 SurfacePageBase = 0;
    uint32 SurfacePlanes = 0; // CBTTerrain::kCBTSurfacePlane* bits; 0 when the surface is not paged
    uint32 Pad = 0;
};
static_assert(sizeof(GrassSurfaceParamsGPU) == 16);

// The page tables of the drawn terrain (terrains[] row 0), as the terrain fragment reads them.
struct SurfacePageSource
{
    uint32 Terrain = 0; // the terrain's render handle
    uint32 Generation = 0;
    uint64 DeviceEpoch = 0;
    float32 WorldOriginX = 0.0f; // the terrain's min corner and size in world meters: UV 0 to 1
    float32 WorldOriginZ = 0.0f;
    float32 WorldSizeX = 0.0f;
    float32 WorldSizeZ = 0.0f;
    std::span<const uint32> HeightWords; // empty when its height is not paged
    std::span<const uint32> SurfaceWords; // empty when its surface is not paged
    uint32 SurfacePlanes = 0;
};

// The terrain UV under the placement's cell window: `span` cells of cellSize meters a side about camera
// cell (cellX, cellZ), as terrain_grass_place.comp walks it, one cell wider on each side for a blade
// that leans across its cell's edge. X runs along U, Y along V.
inline Mathematics::Rect PlacementWindowUV(const SurfacePageSource& source, int32 cellX, int32 cellZ, int32 span,
                                     float32 cellSize)
{
    const int32 halfSpan = (span - 1) / 2;
    const float32 minX = static_cast<float32>(cellX - halfSpan - 1) * cellSize;
    const float32 minZ = static_cast<float32>(cellZ - halfSpan - 1) * cellSize;
    const float32 maxX = static_cast<float32>(cellX - halfSpan + span + 1) * cellSize;
    const float32 maxZ = static_cast<float32>(cellZ - halfSpan + span + 1) * cellSize;
    const float32 invX = 1.0f / std::max(source.WorldSizeX, 0.0001f);
    const float32 invZ = 1.0f / std::max(source.WorldSizeZ, 0.0001f);
    return Mathematics::Rect::FromEdges(Math::Clamp01((minX - source.WorldOriginX) * invX),
                                        Math::Clamp01((minZ - source.WorldOriginZ) * invZ),
                                        Math::Clamp01((maxX - source.WorldOriginX) * invX),
                                        Math::Clamp01((maxZ - source.WorldOriginZ) * invZ));
}

// The params a source publishes. No paged height: no row reads pages.
inline GrassSurfaceParamsGPU BuildSurfaceParams(const SurfacePageSource& source)
{
    GrassSurfaceParamsGPU params{};
    if (source.HeightWords.empty())
        return params;
    // The drawn terrain is terrains[] row 0 (TerrainExtractionSystem publishes the rows in drawn order).
    params.PagedTerrainRow = 0u;
    params.SurfacePageBase = static_cast<uint32>(source.HeightWords.size());
    params.SurfacePlanes = source.SurfaceWords.empty() ? 0u : source.SurfacePlanes;
    return params;
}

} // namespace GameEngine::TerrainGrass
