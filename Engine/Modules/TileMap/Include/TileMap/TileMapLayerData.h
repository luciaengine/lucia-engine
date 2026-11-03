#pragma once

#include "AssetCore/GUID.h"
#include "TileMap/TileMapChunk.h"
#include "TileMap/TileRules.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace GameEngine
{
namespace TileMap
{

enum class TileMapLayerType : uint8
{
    Tiles = 0,
    IntGrid = 1
};

/**
 * @brief One semantic value an IntGrid layer can hold: the small integer that
 * is painted, and how the editor names and tints it.
 *
 * Value 0 is "no value" and is never defined here. The colour is packed
 * 0xRRGGBBAA, matching the terrain definitions the editor swatches alongside it.
 */
struct IntGridValueDef
{
    uint8 Value = 0;
    std::string Name;
    uint32 ColorRGBA = 0xFFFFFFFFu;

    bool operator==(const IntGridValueDef&) const = default;
};

/**
 * @brief One layer of a tilemap: sparse chunked cell storage on an unbounded
 * grid, plus (for IntGrid layers) a parallel plane of semantic uint8 values.
 *
 * Cells are addressed by signed cell coordinates; chunks materialize on first
 * write and are dropped by CompactEmptyChunks() (called before serialization).
 */
class TileMapLayerData
{
public:
    uint32 Id = 0;
    std::string Name;
    TileMapLayerType Type = TileMapLayerType::Tiles;
    GUID TileSetGuid;

    /**
     * Authoring guard: a locked layer refuses paint, so a finished background
     * cannot be scribbled on while its neighbour is the intended target. It
     * lives with the map rather than the entity because it guards editing the
     * map, not how one instance of it renders.
     */
    bool Locked = false;

    /** IntGrid layers: the values that may be painted, in author order. */
    std::vector<IntGridValueDef> IntGridValues;

    /** Tiles layers: the rules that dress this layer from an IntGrid layer. */
    TileRuleSet Rules;

    TileCell GetCell(int32 cellX, int32 cellY) const;
    void SetCell(int32 cellX, int32 cellY, TileCell cell);

    uint8 GetIntGridValue(int32 cellX, int32 cellY) const;
    void SetIntGridValue(int32 cellX, int32 cellY, uint8 value);

    /**
     * Whether this cell was written by rule evaluation. Hand-painted cells
     * report false, and rule evaluation refuses to overwrite them.
     */
    bool IsAutoCell(int32 cellX, int32 cellY) const;
    void SetAutoCell(int32 cellX, int32 cellY, bool automatic);

    const IntGridValueDef* FindIntGridValue(uint8 value) const;

    /** Removes chunks that contain only empty cells / zero values. */
    void CompactEmptyChunks();

    /** Chunk coordinates holding any data, sorted by (Y, X) for stable serialization. */
    std::vector<ChunkCoord> SortedChunkCoords() const;
    std::vector<ChunkCoord> SortedIntGridChunkCoords() const;
    std::vector<ChunkCoord> SortedAutoChunkCoords() const;

    const TileMapChunk* FindChunk(ChunkCoord coord) const;
    const IntGridChunk* FindIntGridChunk(ChunkCoord coord) const;
    const AutoTileChunk* FindAutoChunk(ChunkCoord coord) const;
    TileMapChunk& GetOrCreateChunk(ChunkCoord coord);
    IntGridChunk& GetOrCreateIntGridChunk(ChunkCoord coord);
    AutoTileChunk& GetOrCreateAutoChunk(ChunkCoord coord);

    bool HasAnyCells() const { return !m_Chunks.empty(); }

    void Clear();

private:
    std::unordered_map<uint64, TileMapChunk> m_Chunks;
    std::unordered_map<uint64, IntGridChunk> m_IntGridChunks;
    std::unordered_map<uint64, AutoTileChunk> m_AutoChunks;
};

} // namespace TileMap
} // namespace GameEngine
