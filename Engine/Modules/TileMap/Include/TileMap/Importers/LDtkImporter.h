#pragma once

#include "TileMap/Importers/TileMapImportResult.h"

namespace GameEngine
{
namespace TileMap
{
namespace Importers
{

/**
 * @brief Imports an LDtk project (.ldtk, JSON) into engine tilemap assets:
 * one .tileset per LDtk tileset definition and one .tilemap per level.
 *
 * Scope and conventions:
 *  - Tile and auto layers import their tile RESULTS (gridTiles /
 *    autoLayerTiles). LDtk's rule definitions are NOT reconstructed — the
 *    dressed output is imported as plain hand-painted cells, so re-dressing
 *    in this engine starts from scratch.
 *  - IntGrid layers import their value grid plus the value name/color table;
 *    an IntGrid layer that also carries auto-layer tiles additionally
 *    produces a "<name> (tiles)" Tiles layer with that dressed output.
 *  - LDtk's cell Y grows down; this engine's grows up. Rows are flipped so a
 *    level's bottom row lands at cell Y = 0.
 *  - LDtk flip bits (f: 1 = X, 2 = Y) map to FlipH/FlipV; LDtk has no
 *    rotation, so Transpose is never set.
 *  - Cells where LDtk stacked several auto-layer tiles keep the last (topmost)
 *    tile; the engine stores one cell per coordinate.
 *  - External level files (externalRelPath / .ldtkl) are resolved relative to
 *    the project file and imported like inline levels.
 *  - Levels import in their own coordinate frame (world offsets are ignored);
 *    layer order is preserved as authored (LDtk lists topmost first).
 *
 * Produced assets carry freshly minted GUIDs and default paths next to
 * `ldtkPath`; `resolveTexture` (nullable) maps atlas images to texture GUIDs.
 */
TileMapImportResult ImportLDtkProject(const std::filesystem::path& ldtkPath,
                                      const TextureGuidResolver& resolveTexture);

} // namespace Importers
} // namespace TileMap
} // namespace GameEngine
