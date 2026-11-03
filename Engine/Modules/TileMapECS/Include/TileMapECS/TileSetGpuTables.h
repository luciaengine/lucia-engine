#pragma once

#include "AssetCore/Types.h"

#include <vector>

namespace GameEngine
{
namespace TileMap
{
class TileSetAsset;
} // namespace TileMap

namespace TileMapECS
{

/** Floats per tile in the descriptor SSBO: vec4 UV rect + vec4 animation info. */
inline constexpr size_t kTileDescFloats = 8;
/** Floats per animation frame in the frame SSBO: (uv-source tile index, loop end time). */
inline constexpr size_t kAnimFrameFloats = 2;

/**
 * @brief CPU image of the two SSBOs tilemap.vert reads per tileset.
 *
 * Descs holds kTileDescFloats per tile id (dense, id - 1 indexed): the UV rect
 * in [0..3], then frame count, first frame index and total loop duration in
 * [4..6] ([7] is padding). Frames holds kAnimFrameFloats per frame: the
 * 0-based descriptor index whose UV the frame shows, and the loop time at
 * which the frame ends (cumulative). A tile with frame count 0 is static —
 * the shader's zero-cost path.
 */
struct TileSetGpuTables
{
    std::vector<float32> Descs;
    std::vector<float32> Frames;
    uint32 MaxTileId = 0;
    uint32 AnimatedTileCount = 0;
};

/**
 * Builds the descriptor and frame tables for `tileSet`. Frames referencing
 * tile ids outside the set and animations with a non-positive total duration
 * are dropped (the tile stays static). Returns false when the set has no
 * tiles.
 */
bool BuildTileSetGpuTables(const TileMap::TileSetAsset& tileSet, TileSetGpuTables& outTables);

} // namespace TileMapECS
} // namespace GameEngine
