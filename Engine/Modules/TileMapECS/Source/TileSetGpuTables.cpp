#include "TileMapECS/TileSetGpuTables.h"

#include "TileMap/TileAnimation.h"
#include "TileMap/TileSetAsset.h"

#include <algorithm>

namespace GameEngine
{
namespace TileMapECS
{

bool BuildTileSetGpuTables(const TileMap::TileSetAsset& tileSet, TileSetGpuTables& outTables)
{
    outTables.Descs.clear();
    outTables.Frames.clear();
    outTables.MaxTileId = 0;
    outTables.AnimatedTileCount = 0;

    for (const TileMap::TileSetTile& tile : tileSet.Tiles())
        outTables.MaxTileId = std::max(outTables.MaxTileId, tile.Id);
    if (outTables.MaxTileId == 0)
        return false;

    outTables.Descs.assign(static_cast<size_t>(outTables.MaxTileId) * kTileDescFloats, 0.0f);

    for (const TileMap::TileSetTile& tile : tileSet.Tiles())
    {
        if (tile.Id == 0)
            continue;
        float32* desc = outTables.Descs.data() + static_cast<size_t>(tile.Id - 1) * kTileDescFloats;
        desc[0] = tile.UV.U0;
        desc[1] = tile.UV.V0;
        desc[2] = tile.UV.U1;
        desc[3] = tile.UV.V1;

        if (tile.AnimationFrames.empty())
            continue;

        const uint32 firstFrame =
            static_cast<uint32>(outTables.Frames.size() / kAnimFrameFloats);
        uint32 frameCount = 0;
        float32 loopEnd = 0.0f;
        for (const TileMap::TileAnimationFrame& frame : tile.AnimationFrames)
        {
            // A frame naming a tile outside the set has no UV to show.
            if (frame.TileId == 0 || frame.TileId > outTables.MaxTileId)
                continue;
            loopEnd += std::max(0.0f, frame.DurationSeconds);
            outTables.Frames.push_back(static_cast<float32>(frame.TileId - 1));
            outTables.Frames.push_back(loopEnd);
            ++frameCount;
        }

        if (frameCount == 0 || loopEnd <= 0.0f)
        {
            // Nothing playable; drop what was appended and leave the tile static.
            outTables.Frames.resize(static_cast<size_t>(firstFrame) * kAnimFrameFloats);
            continue;
        }
        desc[4] = static_cast<float32>(frameCount);
        desc[5] = static_cast<float32>(firstFrame);
        desc[6] = loopEnd;
        ++outTables.AnimatedTileCount;
    }

    // The frames SSBO binding needs a non-empty buffer even for a fully static
    // tileset; one dummy frame keeps the bind unconditional.
    if (outTables.Frames.empty())
        outTables.Frames.assign(kAnimFrameFloats, 0.0f);
    return true;
}

} // namespace TileMapECS
} // namespace GameEngine
