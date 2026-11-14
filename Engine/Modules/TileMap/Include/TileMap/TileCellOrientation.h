#pragma once

#include "TileMap/TileCellEncoding.h"

namespace GameEngine
{
namespace TileMap
{

/**
 * @brief The eight orientations a cell's flip/transpose bits encode, and the
 * dihedral group operations that compose onto them.
 *
 * The bits follow the Tiled GID convention: transpose (reflection across the
 * main diagonal) applies first, then horizontal flip, then vertical flip. That
 * ordering makes naive bit-toggling wrong for anything but the identity — a
 * horizontal flip of an already-transposed cell must become a *vertical* flip.
 * These helpers compose in the group instead, so hotkeys stay correct through
 * any sequence of flips and rotations.
 */

/** Mirrors the cell across its vertical axis, preserving the tile id. */
TileCell FlipCellHorizontal(TileCell cell);

/** Mirrors the cell across its horizontal axis, preserving the tile id. */
TileCell FlipCellVertical(TileCell cell);

/** Rotates the cell a quarter turn clockwise, preserving the tile id. */
TileCell RotateCellClockwise(TileCell cell);

} // namespace TileMap
} // namespace GameEngine
