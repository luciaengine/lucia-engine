#pragma once

#include "AssetCore/GUID.h"
#include "TileMap/TileMapAsset.h"
#include "TileMap/TileSetAsset.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace GameEngine
{
namespace TileMap
{
namespace Importers
{

// Hard ceilings on hostile or corrupt input. Every count read from a foreign
// file is checked against one of these before any allocation sized by it, and
// counts that accumulate — cells across a layer's chunks — are checked against
// their running total, not one element at a time.
inline constexpr int32 kMaxImportCellsPerAxis = 16384;
inline constexpr int64 kMaxImportCellsPerLayer = int64(1) << 22; // 4M cells
// Cell coordinates stay well inside int32 so a chunk origin plus its extent
// cannot overflow.
inline constexpr int32 kMaxImportCellCoordinate = 1 << 24;
inline constexpr int32 kMaxImportTilePixels = 16384;
inline constexpr int32 kMaxImportLayers = 256;
inline constexpr int32 kMaxImportLayerDepth = 64;
inline constexpr int32 kMaxImportTileSets = 64;
inline constexpr int32 kMaxImportTilesPerSet = kMaxTileSetTiles;
inline constexpr int32 kMaxImportLevels = 256;
inline constexpr int32 kMaxImportChunksPerLayer = 65536;
inline constexpr int32 kMaxImportAnimationFrames = 1024;
inline constexpr int32 kMaxImportPolygonPoints = 256;
// Whole-import ceilings, shared across levels, layers, tilesets and external files.
inline constexpr uint64 kMaxImportSourceBytes = uint64(64) << 20;
inline constexpr uint64 kMaxImportItems = uint64(1) << 20;
inline constexpr uint64 kMaxImportCells = uint64(16) << 20;
inline constexpr uint64 kMaxImportOutputChunks = 16384;

/**
 * Maps an atlas image file to the engine texture GUID a produced .tileset
 * should reference. The editor resolves through the asset registry; headless
 * callers (tests) supply a fixed mapping. A null return leaves the tileset's
 * atlas GUID null (imported, but untextured until assigned).
 */
using TextureGuidResolver = std::function<GUID(const std::filesystem::path& absoluteImagePath)>;

/** One .tileset produced by an import, under a freshly minted GUID. */
struct ImportedTileSet
{
    GUID Guid;
    std::string Name;                  // suggested asset file stem
    std::filesystem::path ImagePath;   // absolute atlas image path (may not exist)
    std::unique_ptr<TileSetAsset> Asset;
};

/** One .tilemap produced by an import; layers reference ImportedTileSet GUIDs. */
struct ImportedTileMap
{
    std::string Name; // suggested asset file stem
    std::unique_ptr<TileMapAsset> Asset;
};

/**
 * Result of a source-file import: either Success with the produced assets, or
 * a single fatal Error. Non-fatal degradations (skipped cells, unsupported
 * shapes) land in Warnings so a caller can surface them without failing.
 */
struct TileMapImportResult
{
    bool Success = false;
    std::string Error;
    std::vector<std::string> Warnings;
    std::vector<ImportedTileSet> TileSets;
    std::vector<ImportedTileMap> Maps;
};

/**
 * Rewrites every layer tileset reference `from` to `to` across the imported
 * maps — for callers whose asset registry mints the final GUID at
 * registration time rather than accepting the imported one.
 */
void RemapImportedTileSetGuid(TileMapImportResult& result, const GUID& from, const GUID& to);

} // namespace Importers
} // namespace TileMap
} // namespace GameEngine
