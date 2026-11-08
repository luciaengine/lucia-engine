#pragma once

#include "Terrain/Heightfield.h"
#include "Types/Types.h"

#include <vector>

namespace GameEngine::TerrainECS
{

/// Full-resolution heights and normals prepared by the coarse streaming job.
/// Extraction copies these texels until full detail arrives. Normals use no neighbor tiles.
struct CoarseTileStandIn
{
    Terrain::HeightfieldData Heights; ///< tileRes x tileRes
    std::vector<uint8> Normalmap;     ///< R16G16_FLOAT, tileRes x tileRes
    float32 TileWorldSize = 0.0f;     ///< the extent the normal was computed over, in meters
    float32 HeightScale = 0.0f;       ///< the height scale the normal was computed with
    uint64 HeightfieldVersion = 0;    ///< the tile's HeightfieldVersion these texels belong to

    /// True when this stand-in is the one a tile of this resolution, extent, height scale and
    /// heightfield version would build now.
    bool Matches(uint32 tileRes, float32 tileWorldSize, float32 heightScale, uint64 heightfieldVersion) const;
};

/// Builds `out` from a coarse heightfield: the upsample to tileRes and its normal. Byte-identical
/// whichever thread runs it. Leaves `out` empty when the coarse field or tileRes is below 2.
void BuildCoarseTileStandIn(const Terrain::HeightfieldData& coarse, uint32 tileRes, float32 tileWorldSize,
                            float32 heightScale, CoarseTileStandIn& out);

} // namespace GameEngine::TerrainECS
