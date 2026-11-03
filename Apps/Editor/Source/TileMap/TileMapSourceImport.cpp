#include "TileMap/TileMapSourceImport.h"

#include "FileSystem/FileSystem.h"
#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Core/Engine.h"
#include "Logger/Logger.h"
#include "TileMap/Importers/LDtkImporter.h"
#include "TileMap/Importers/TiledImporter.h"
#include "Types/StringUtils.h"

#include <fstream>
#include <system_error>

namespace GameEngine
{
namespace Editor
{

namespace
{
namespace Importers = ::GameEngine::TileMap::Importers;

bool PublishImportedAsset(const std::filesystem::path& path, const Vector<uint8>& bytes)
{
    std::error_code directoryError;
    std::filesystem::create_directories(path.parent_path(), directoryError);
    if (directoryError)
        return false;
    const auto temporary = FileSystem::MakeTemporarySiblingPath(path);
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    output.close();
    if (!output.good())
    {
        std::error_code removeError;
        std::filesystem::remove(temporary, removeError);
        return false;
    }
    return FileSystem::PublishFile(temporary, path);
}

std::string PathText(const std::filesystem::path& path)
{
    const std::u8string text = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

} // namespace

bool IsTileMapSourceFile(const std::filesystem::path& path)
{
    const std::string ext = ToLowerAscii(PathText(path.extension()));
    return ext == ".tmx" || ext == ".ldtk";
}

std::vector<std::filesystem::path> ExistingTileMapImportOutputs(const Importers::TileMapImportResult& imported)
{
    std::vector<std::filesystem::path> existing;
    const auto check = [&existing](const std::filesystem::path& path) {
        std::error_code error;
        const auto status = std::filesystem::symlink_status(path, error);
        if (status.type() != std::filesystem::file_type::not_found ||
            (error && error != std::errc::no_such_file_or_directory))
            existing.push_back(path);
    };
    for (const Importers::ImportedTileSet& tileSet : imported.TileSets)
        check(tileSet.Asset->GetPath());
    for (const Importers::ImportedTileMap& map : imported.Maps)
        check(map.Asset->GetPath());
    return existing;
}

bool WriteTileMapImport(Importers::TileMapImportResult& imported, bool overwrite,
                        const TileMapImportRegistrar& registerAsset, std::string& outError,
                        TileMapImportSummary* outSummary)
{
    outError.clear();
    if (outSummary)
    {
        *outSummary = {};
        outSummary->Warnings = imported.Warnings;
    }
    if (!imported.Success || !registerAsset)
    {
        outError = imported.Error.empty() ? "Invalid tile-map import or asset registrar" : imported.Error;
        return false;
    }
    if (!overwrite)
    {
        std::vector<std::filesystem::path> existing = ExistingTileMapImportOutputs(imported);
        if (!existing.empty())
        {
            std::string names;
            for (const std::filesystem::path& path : existing)
                names += (names.empty() ? "" : ", ") + PathText(path.filename());
            outError = "The import would replace " + names +
                       ". Nothing was written; confirm the overwrite to replace them (painting and layers "
                       "added to a .tilemap after its import are lost).";
            if (outSummary)
                outSummary->ExistingOutputs = std::move(existing);
            return false;
        }
    }

    // Write and register the tilesets first: the registry mints the durable
    // GUID at registration, and the maps must reference exactly that GUID.
    for (Importers::ImportedTileSet& tileSet : imported.TileSets)
    {
        const std::filesystem::path path = tileSet.Asset->GetPath();
        Vector<uint8> bytes;
        if (!tileSet.Asset->SaveToData(bytes) || !PublishImportedAsset(path, bytes))
        {
            outError = "Failed to write " + PathText(path);
            return false;
        }
        if (outSummary)
            outSummary->TileSetPaths.push_back(path);
        const GUID registered = registerAsset(path);
        if (registered.IsNull())
        {
            outError = "Failed to register " + PathText(path) + " (is the file inside the project?)";
            return false;
        }
        if (registered != tileSet.Guid)
            Importers::RemapImportedTileSetGuid(imported, tileSet.Guid, registered);
    }

    for (Importers::ImportedTileMap& map : imported.Maps)
    {
        const std::filesystem::path path = map.Asset->GetPath();
        Vector<uint8> bytes;
        if (!map.Asset->SaveToData(bytes) || !PublishImportedAsset(path, bytes))
        {
            outError = "Failed to write " + PathText(path);
            return false;
        }
        if (outSummary)
            outSummary->MapPaths.push_back(path);
        if (registerAsset(path).IsNull())
        {
            outError = "Failed to register " + PathText(path) + " (is the file inside the project?)";
            return false;
        }
    }

    if (outSummary)
        outSummary->Warnings = imported.Warnings;
    return true;
}

bool ImportTileMapSource(const std::filesystem::path& sourcePath, bool overwrite, std::string& outError,
                         TileMapImportSummary* outSummary)
{
    outError.clear();
    if (outSummary)
        *outSummary = {};
    auto& engine = EngineCore::GetInstance();
    if (!engine.IsInitialized())
    {
        outError = "Engine not initialized";
        return false;
    }
    AssetRegistry& registry = engine.GetAssetManager().GetRegistry();

    // Atlas images resolve through the registry (registering them on first
    // sight), so the produced tilesets reference real texture assets.
    const Importers::TextureGuidResolver resolveTexture =
        [&registry](const std::filesystem::path& imagePath) -> GUID
    {
        const auto result = registry.RegisterAssetByPath(imagePath);
        return result.IsOk() ? result.Value() : GUID();
    };

    const std::string ext = ToLowerAscii(PathText(sourcePath.extension()));
    Importers::TileMapImportResult imported;
    if (ext == ".tmx")
        imported = Importers::ImportTiledMap(sourcePath, resolveTexture);
    else if (ext == ".ldtk")
        imported = Importers::ImportLDtkProject(sourcePath, resolveTexture);
    else
    {
        outError = "Not a Tiled (.tmx) or LDtk (.ldtk) file: " + PathText(sourcePath);
        return false;
    }
    if (!imported.Success)
    {
        outError = imported.Error;
        return false;
    }

    const TileMapImportRegistrar registerAsset = [&registry](const std::filesystem::path& path) -> GUID
    {
        const auto registered = registry.RegisterAssetByPath(path);
        return registered.IsOk() ? registered.Value() : GUID();
    };
    if (!WriteTileMapImport(imported, overwrite, registerAsset, outError, outSummary))
        return false;

    for (const std::string& warning : imported.Warnings)
        Logger::Log::Warning("TileMapSourceImport: {}", warning);
    Logger::Log::Info("TileMapSourceImport: imported '{}' ({} tilesets, {} maps)",
                      PathText(sourcePath), imported.TileSets.size(), imported.Maps.size());
    return true;
}

} // namespace Editor
} // namespace GameEngine
