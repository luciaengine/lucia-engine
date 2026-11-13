#pragma once

#include "Types/Types.h"

#include <memory>
#include <vector>

namespace GameEngine::PageStreaming
{
class PageStoreAccess;
}

namespace GameEngine::TerrainECS
{

struct TerrainHeightSourceInputs;

/// The surface stores of a tiled terrain as its surface page stream reads them. The surface page
/// driver (TerrainSurfacePageDriver) sets it on TiledTerrainData each frame.
struct TerrainSurfacePageSource
{
    /// One store access per plane, in PageFieldKind order (Splat, Normal, GrassControl, Color).
    std::vector<std::shared_ptr<const PageStreaming::PageStoreAccess>> Planes;
    /// Bit (kind - PageFieldKind::Splat) of each plane in Planes.
    uint32 PlaneMask = 0;
    /// The stores' open job wrote every plane's pinned levels: each coarse page is readable.
    bool PinnedLevelsBaked = false;
};

/// True when the terrain's surface is read from its surface pages: its height is paged
/// (TerrainHeightIsPaged, which needs the wide CBT arm) and every plane's pinned levels are baked.
/// Otherwise it keeps its unified or atlas surface textures.
bool TerrainSurfaceIsPaged(const TerrainHeightSourceInputs& height, const TerrainSurfacePageSource& surface);

} // namespace GameEngine::TerrainECS
