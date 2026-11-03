#include "TileMap/TileCellOrientation.h"

namespace GameEngine
{
namespace TileMap
{
namespace
{

/**
 * A dihedral element as its 2x2 linear map on tile-centered coordinates, where
 * u/v run [-1,1] about the tile center. Every entry is -1, 0 or 1, so the
 * composition below is exact integer arithmetic.
 */
struct Orientation2x2
{
    int32 M00 = 1;
    int32 M01 = 0;
    int32 M10 = 0;
    int32 M11 = 1;
};

/**
 * Decodes the cell's flip/transpose bits into their linear map. The bit order
 * is transpose, then horizontal, then vertical, so the matrix is
 * Vertical * Horizontal * Transpose.
 */
Orientation2x2 ToMatrix(TileCell cell)
{
    const int32 h = CellFlipH(cell) ? -1 : 1;
    const int32 v = CellFlipV(cell) ? -1 : 1;

    if (!CellTranspose(cell))
        return Orientation2x2{h, 0, 0, v};

    // Transpose swaps the axes, moving the flip signs onto the anti-diagonal.
    return Orientation2x2{0, h, v, 0};
}

/** Inverse of ToMatrix: reads the flip/transpose bits back out of the map. */
TileCell ApplyMatrix(TileCell cell, const Orientation2x2& m)
{
    const bool transpose = (m.M00 == 0);
    const int32 h = transpose ? m.M01 : m.M00;
    const int32 v = transpose ? m.M10 : m.M11;
    return MakeCell(CellTileId(cell), h < 0, v < 0, transpose);
}

/** Returns `op * current` — op applied after the cell's existing orientation. */
Orientation2x2 Compose(const Orientation2x2& op, const Orientation2x2& current)
{
    return Orientation2x2{
        op.M00 * current.M00 + op.M01 * current.M10,
        op.M00 * current.M01 + op.M01 * current.M11,
        op.M10 * current.M00 + op.M11 * current.M10,
        op.M10 * current.M01 + op.M11 * current.M11};
}

constexpr Orientation2x2 kFlipHorizontal{-1, 0, 0, 1};
constexpr Orientation2x2 kFlipVertical{1, 0, 0, -1};
// A quarter turn clockwise in a Y-up frame: (u,v) -> (v,-u).
constexpr Orientation2x2 kRotateClockwise{0, 1, -1, 0};

TileCell ApplyOperation(TileCell cell, const Orientation2x2& op)
{
    if (CellIsEmpty(cell))
        return kEmptyCell;
    return ApplyMatrix(cell, Compose(op, ToMatrix(cell)));
}

} // namespace

TileCell FlipCellHorizontal(TileCell cell) { return ApplyOperation(cell, kFlipHorizontal); }

TileCell FlipCellVertical(TileCell cell) { return ApplyOperation(cell, kFlipVertical); }

TileCell RotateCellClockwise(TileCell cell) { return ApplyOperation(cell, kRotateClockwise); }

} // namespace TileMap
} // namespace GameEngine
