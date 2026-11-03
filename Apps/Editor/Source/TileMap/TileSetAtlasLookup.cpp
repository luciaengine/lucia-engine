#include "TileMap/TileSetAtlasLookup.h"

#include "Assets/AssetRegistry.h"
#include "TileMap/TileSetAsset.h"

namespace GameEngine
{
namespace Editor
{

std::filesystem::path FindTileSetForAtlas(AssetRegistry& registry, const GUID& atlasTexture,
                                          const std::filesystem::path& preferredPath)
{
    if (atlasTexture.IsNull())
        return {};

    std::filesystem::path best;
    for (const GUID& guid : registry.GetAssetsByType(AssetType::TileSet))
    {
        AssetMetadata metadata;
        if (!registry.TryGetAssetMetadata(guid, metadata) || metadata.Path.empty())
            continue;

        // A tile set that fails to parse is skipped rather than fatal: the point
        // of this lookup is to find the author's file, and one unreadable
        // neighbour must not push the caller back onto the stem guess.
        TileMap::TileSetAsset asset(guid, metadata.Path);
        bool loaded = false;
        try
        {
            loaded = asset.Load();
        }
        catch (...)
        {
            loaded = false;
        }
        if (!loaded || asset.AtlasTexture() != atlasTexture)
            continue;

        if (!preferredPath.empty() && metadata.Path == preferredPath)
            return metadata.Path;
        if (best.empty() || metadata.Path < best)
            best = metadata.Path;
    }
    return best;
}

} // namespace Editor
} // namespace GameEngine
