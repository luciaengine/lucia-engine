#pragma once

#include "TileMap/TileCellEncoding.h"

#include <array>

namespace GameEngine
{
namespace TileMap
{

inline constexpr int32 kChunkSizeLog2 = 5;
inline constexpr int32 kChunkSize = 1 << kChunkSizeLog2; // 32
inline constexpr int32 kChunkCellCount = kChunkSize * kChunkSize;
inline constexpr int32 kChunkLocalMask = kChunkSize - 1;

/**
 * @brief Integer chunk coordinate on the unbounded cell grid.
 */
struct ChunkCoord
{
    int32 X = 0;
    int32 Y = 0;

    bool operator==(const ChunkCoord&) const = default;
};

/**
 * @brief Integer cell coordinate on the unbounded cell grid. Y grows upward,
 * matching the peering slot offsets.
 */
struct CellCoord
{
    int32 X = 0;
    int32 Y = 0;

    bool operator==(const CellCoord&) const = default;
};

/** Packs a chunk coordinate into a single map key. */
constexpr uint64 PackChunkCoord(ChunkCoord coord)
{
    return (static_cast<uint64>(static_cast<uint32>(coord.X)) << 32) |
           static_cast<uint64>(static_cast<uint32>(coord.Y));
}

constexpr ChunkCoord UnpackChunkCoord(uint64 packed)
{
    return ChunkCoord{static_cast<int32>(static_cast<uint32>(packed >> 32)),
                      static_cast<int32>(static_cast<uint32>(packed))};
}

/** Packs a cell coordinate into a single map key. */
constexpr uint64 PackCellCoord(CellCoord coord)
{
    return (static_cast<uint64>(static_cast<uint32>(coord.X)) << 32) |
           static_cast<uint64>(static_cast<uint32>(coord.Y));
}

constexpr CellCoord UnpackCellCoord(uint64 packed)
{
    return CellCoord{static_cast<int32>(static_cast<uint32>(packed >> 32)),
                     static_cast<int32>(static_cast<uint32>(packed))};
}

/** Chunk containing a given cell (arithmetic shift = floor division, correct for negatives). */
constexpr ChunkCoord ChunkOfCell(int32 cellX, int32 cellY)
{
    return ChunkCoord{cellX >> kChunkSizeLog2, cellY >> kChunkSizeLog2};
}

/** Cell position within its chunk, in [0, kChunkSize). */
constexpr int32 LocalCellX(int32 cellX) { return cellX & kChunkLocalMask; }
constexpr int32 LocalCellY(int32 cellY) { return cellY & kChunkLocalMask; }
constexpr int32 LocalCellIndex(int32 cellX, int32 cellY)
{
    return LocalCellY(cellY) * kChunkSize + LocalCellX(cellX);
}

/**
 * @brief Fixed 32x32 plane of tile cells, row-major. Value-zero cells are empty.
 */
struct TileMapChunk
{
    std::array<TileCell, kChunkCellCount> Cells{};

    bool IsAllEmpty() const
    {
        for (TileCell cell : Cells)
        {
            if (!CellIsEmpty(cell))
                return false;
        }
        return true;
    }
};

/**
 * @brief Fixed 32x32 plane of IntGrid values, row-major. Zero = no value.
 */
struct IntGridChunk
{
    std::array<uint8, kChunkCellCount> Values{};

    bool IsAllZero() const
    {
        for (uint8 value : Values)
        {
            if (value != 0)
                return false;
        }
        return true;
    }
};

/**
 * @brief Fixed 32x32 provenance bitplane, one word per row: a set bit marks a
 * cell written by rule evaluation rather than by hand.
 *
 * Rule evaluation may only overwrite auto cells and empty cells, so this plane
 * is what keeps a hand-painted detail alive through a re-dress of its region.
 * A row is exactly kChunkSize bits wide, which is why one uint32 per row is the
 * whole storage.
 */
struct AutoTileChunk
{
    static_assert(kChunkSize == 32, "AutoTileChunk packs one chunk row per uint32");

    std::array<uint32, kChunkSize> Rows{};

    bool Get(int32 cellX, int32 cellY) const
    {
        return (Rows[LocalCellY(cellY)] & (1u << LocalCellX(cellX))) != 0u;
    }

    void Set(int32 cellX, int32 cellY, bool automatic)
    {
        const uint32 bit = 1u << LocalCellX(cellX);
        if (automatic)
            Rows[LocalCellY(cellY)] |= bit;
        else
            Rows[LocalCellY(cellY)] &= ~bit;
    }

    bool IsAllZero() const
    {
        for (uint32 row : Rows)
        {
            if (row != 0u)
                return false;
        }
        return true;
    }
};

} // namespace TileMap
} // namespace GameEngine
