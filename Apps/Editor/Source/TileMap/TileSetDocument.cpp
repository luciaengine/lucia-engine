#include "TileMap/TileSetDocument.h"

#include "FileSystem/FileSystem.h"
#include "Types/Fnv1a.h"
#include "Logger/Logger.h"
#include "TileMap/TileSetAsset.h"

#include <fstream>
#include <system_error>

namespace GameEngine
{
namespace Editor
{
namespace
{

constexpr const char* kTileSetExtension = ".tileset";

/** FNV-1a over the file's bytes; 0 when there is no readable file. */
uint64_t HashFileBytes(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open())
        return 0;
    uint64_t hash = Hashing::kFnv1a64OffsetBasis;
    char buffer[4096];
    while (in.read(buffer, sizeof(buffer)) || in.gcount() > 0)
    {
        const std::streamsize count = in.gcount();
        hash = Hashing::Fnv1a64(buffer, static_cast<size_t>(count), hash);
        if (count < static_cast<std::streamsize>(sizeof(buffer)))
            break;
    }
    return hash;
}

bool TileCarriesAuthoring(const TileMap::TileSetTile& tile)
{
    return !tile.CollisionPolygons.empty() ||
           tile.CollisionMode != TileMap::TileCollisionMode::Solid ||
           !tile.PhysicsMaterial.IsNull() || !tile.AnimationFrames.empty() ||
           tile.Peering.ParticipatesInTerrain() || !tile.CustomData.empty() ||
           !tile.TextureOverride.IsNull() || tile.Probability != 1.0f;
}

} // namespace

TileSetDocument::TileSetDocument() = default;
TileSetDocument::~TileSetDocument() = default;

std::filesystem::path TileSetDocument::PathForAtlasTexture(const std::filesystem::path& texturePath)
{
    if (texturePath.empty())
        return {};
    return std::filesystem::path(texturePath).replace_extension(kTileSetExtension);
}

TileSetDocument::OpenOutcome TileSetDocument::OpenOrCreate(const GUID& guid,
                                                           const std::filesystem::path& path)
{
    if (path.empty())
        return OpenOutcome::Failed;

    std::error_code ec;
    const bool exists = std::filesystem::exists(path, ec) && !ec;

    auto asset = std::make_unique<TileMap::TileSetAsset>(guid, path);
    bool loaded = true;
    if (exists)
    {
        // The JSON decode throws on malformed input, and the loader's own catch
        // does not fire in every engine build, so failing to bind is contained
        // here — an unreadable file must leave the author's file alone rather
        // than take the editor down or fall through to a blank asset.
        try
        {
            loaded = asset->Load();
        }
        catch (...)
        {
            loaded = false;
        }
    }
    if (!loaded)
    {
        Logger::Log::Error("TileSetDocument: cannot read '{}'", path.string());
        return OpenOutcome::Failed;
    }

    m_Asset = std::move(asset);
    m_Path = path;
    m_DiskContentHash = exists ? HashFileBytes(path) : 0;
    return exists ? OpenOutcome::Opened : OpenOutcome::Created;
}

TileSetDocument::SaveOutcome TileSetDocument::Save()
{
    if (!m_Asset || m_Path.empty())
        return SaveOutcome::Failed;

    if (HasChangedOnDisk())
    {
        Logger::Log::Warning("TileSetDocument: '{}' changed on disk; not overwriting it",
                             m_Path.string());
        return SaveOutcome::ChangedOnDisk;
    }

    Vector<uint8> bytes;
    if (!m_Asset->SaveToData(bytes))
    {
        Logger::Log::Error("TileSetDocument: failed to write '{}'", m_Path.string());
        return SaveOutcome::Failed;
    }
    std::error_code directoryError;
    std::filesystem::create_directories(m_Path.parent_path(), directoryError);
    const auto temporary = FileSystem::MakeTemporarySiblingPath(m_Path);
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    output.close();
    if (!output.good())
    {
        std::error_code removeError;
        std::filesystem::remove(temporary, removeError);
        return SaveOutcome::Failed;
    }
    if (!FileSystem::PublishFile(temporary, m_Path))
        return SaveOutcome::Failed;
    m_DiskContentHash = Hashing::Fnv1a64(bytes.data(), bytes.size());
    return SaveOutcome::Written;
}

bool TileSetDocument::HasChangedOnDisk() const
{
    // Whoever wrote the file after this document read it put changes in it that
    // are not in this copy; overwriting would drop them with no trace. A hash of
    // 0 means "there was no file", so this also catches one appearing since.
    return !m_Path.empty() && HashFileBytes(m_Path) != m_DiskContentHash;
}

void TileSetDocument::AcceptDiskState()
{
    m_DiskContentHash = m_Path.empty() ? 0 : HashFileBytes(m_Path);
}

bool TileSetDocument::HasAuthoredTileData() const
{
    if (!m_Asset)
        return false;
    for (const TileMap::TileSetTile& tile : m_Asset->Tiles())
    {
        if (TileCarriesAuthoring(tile))
            return true;
    }
    return false;
}

bool TileSetDocument::ReSliceDropsAuthoring(int32_t tileWidthPx, int32_t tileHeightPx,
                                            int32_t marginPx, int32_t spacingPx) const
{
    if (!m_Asset || !HasAuthoredTileData())
        return false;

    const int32_t columns = TileMap::TileSetAsset::GridColumns(m_Asset->AtlasWidthPx(), tileWidthPx,
                                                               marginPx, spacingPx);
    const int32_t rows = TileMap::TileSetAsset::GridRows(m_Asset->AtlasHeightPx(), tileHeightPx,
                                                         marginPx, spacingPx);
    return columns != m_Asset->Columns() || rows != m_Asset->Rows();
}

} // namespace Editor
} // namespace GameEngine
