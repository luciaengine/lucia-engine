#pragma once

#include "AssetCore/Types.h"

namespace GameEngine
{
namespace TileMap
{

/**
 * @brief One tilemap cell, packed into 32 bits.
 *
 * Bits 0..28 hold the tile id (1-based into the layer's tileset; 0 = empty
 * cell). Bits 29/30/31 are horizontal flip, vertical flip, and transpose —
 * the Tiled GID convention, giving all 8 dihedral orientations without
 * duplicating tiles.
 */
using TileCell = uint32;

inline constexpr TileCell kEmptyCell = 0;
inline constexpr uint32 kCellTileIdBits = 29;
inline constexpr TileCell kCellTileIdMask = (TileCell(1) << kCellTileIdBits) - 1;
inline constexpr TileCell kCellFlipHBit = TileCell(1) << 29;
inline constexpr TileCell kCellFlipVBit = TileCell(1) << 30;
inline constexpr TileCell kCellTransposeBit = TileCell(1) << 31;
inline constexpr TileCell kCellFlagsMask = kCellFlipHBit | kCellFlipVBit | kCellTransposeBit;
inline constexpr uint32 kMaxTileId = kCellTileIdMask;

constexpr TileCell MakeCell(uint32 tileId, bool flipH = false, bool flipV = false, bool transpose = false)
{
    TileCell cell = tileId & kCellTileIdMask;
    if (flipH)
        cell |= kCellFlipHBit;
    if (flipV)
        cell |= kCellFlipVBit;
    if (transpose)
        cell |= kCellTransposeBit;
    return cell;
}

constexpr uint32 CellTileId(TileCell cell) { return cell & kCellTileIdMask; }
constexpr bool CellFlipH(TileCell cell) { return (cell & kCellFlipHBit) != 0; }
constexpr bool CellFlipV(TileCell cell) { return (cell & kCellFlipVBit) != 0; }
constexpr bool CellTranspose(TileCell cell) { return (cell & kCellTransposeBit) != 0; }
constexpr bool CellIsEmpty(TileCell cell) { return CellTileId(cell) == 0; }

} // namespace TileMap
} // namespace GameEngine
