#pragma once

#include "AssetCore/GUID.h"
#include "TileMap/Importers/TileMapImportResult.h"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace GameEngine
{
namespace Editor
{

/** What an import produced, for status lines and scripted verification. */
struct TileMapImportSummary
{
    std::vector<std::filesystem::path> TileSetPaths;
    std::vector<std::filesystem::path> MapPaths;
    std::vector<std::string> Warnings;
    // Files already on disk that the import would replace. Filled when the
    // import is refused for them; nothing was written then.
    std::vector<std::filesystem::path> ExistingOutputs;
};

/** True for source files ImportTileMapSource understands (.tmx / .ldtk). */
bool IsTileMapSourceFile(const std::filesystem::path& path);

/** The .tileset/.tilemap files an import would write that already exist. */
std::vector<std::filesystem::path> ExistingTileMapImportOutputs(
    const TileMap::Importers::TileMapImportResult& imported);

/**
 * Registers a written asset file and returns the GUID the registry holds for
 * it, or a null GUID when the file could not be registered.
 */
using TileMapImportRegistrar = std::function<GUID(const std::filesystem::path& path)>;

/**
 * Writes an import's .tileset and .tilemap files and registers each one;
 * maps are rewritten to reference the GUIDs the registrar returns. A .tilemap
 * holds painting, layers and IntGrid data the source file does not, so
 * without `overwrite` an import that would replace an existing file is
 * refused before anything is written, and `outError` names the files.
 */
bool WriteTileMapImport(TileMap::Importers::TileMapImportResult& imported, bool overwrite,
                        const TileMapImportRegistrar& registerAsset, std::string& outError,
                        TileMapImportSummary* outSummary = nullptr);

/**
 * Imports a Tiled (.tmx) or LDtk (.ldtk) source file into engine assets
 * written next to it, through the asset registry so GUIDs and thumbnails
 * appear: atlas images and produced .tileset/.tilemap files are registered,
 * and map layers reference the registry-minted tileset GUIDs.
 *
 * Returns false with `outError` set on failure, and when existing outputs
 * would be replaced without `overwrite` (listed in the summary's
 * ExistingOutputs). On success `outSummary` (optional) lists what was written
 * plus non-fatal importer warnings.
 */
bool ImportTileMapSource(const std::filesystem::path& sourcePath, bool overwrite, std::string& outError,
                         TileMapImportSummary* outSummary = nullptr);

} // namespace Editor
} // namespace GameEngine
