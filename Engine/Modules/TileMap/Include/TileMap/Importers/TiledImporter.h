#pragma once

#include "TileMap/Importers/TileMapImportResult.h"

namespace GameEngine
{
namespace TileMap
{
namespace Importers
{

/**
 * @brief Imports a Tiled map (.tmx, XML) into engine tilemap assets: one
 * .tileset per referenced tileset (inline or external .tsx) and one .tilemap.
 *
 * Scope and conventions:
 *  - Orthogonal maps only; finite and infinite (<chunk>) layouts.
 *  - Layer data: CSV, plain base64, and base64+zlib/gzip. zstd is not
 *    supported.
 *  - Tiled GID flip flags map 1:1 onto this engine's cell flags (H, V,
 *    diagonal = Transpose — the same dihedral composition order; the bit
 *    positions differ and are re-packed).
 *  - Tiled's cell Y grows down; this engine's grows up. Finite maps flip so
 *    the bottom row lands at cell Y = 0; infinite maps negate Y around the
 *    origin row (row 0 stays 0).
 *  - Per-tile animations and objectgroup collision shapes (rects and
 *    polygons; ellipses and rotated objects are skipped with a warning)
 *    import into the tileset. Per-tile properties import as custom data.
 *  - A layer drawing from several tilesets is split into one sublayer per
 *    tileset, named "Layer (tilesetName)" — an engine layer binds one
 *    tileset.
 *  - Image-collection tilesets (per-tile images) are rejected.
 *
 * Produced assets carry freshly minted GUIDs and default paths next to
 * `tmxPath`; `resolveTexture` (nullable) maps atlas images to texture GUIDs.
 */
TileMapImportResult ImportTiledMap(const std::filesystem::path& tmxPath,
                                   const TextureGuidResolver& resolveTexture);

} // namespace Importers
} // namespace TileMap
} // namespace GameEngine
