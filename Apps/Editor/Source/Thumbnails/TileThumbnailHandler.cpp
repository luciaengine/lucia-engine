#include "Thumbnails/TileThumbnailHandler.h"

#include "Thumbnails/ThumbnailImageCache.h"
#include "Thumbnails/TileMapThumbnailComposite.h"

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Logger/Logger.h"
#include "TileMap/TileCellEncoding.h"
#include "TileMap/TileMapAsset.h"
#include "TileMap/TileMapChunk.h"
#include "TileMap/TileMapData.h"
#include "TileMap/TileMapLayerData.h"
#include "TileMap/TileSetAsset.h"
#include "Types/StringUtils.h"

#include <algorithm>
#include <cstdint>
#include "JobSystem/WorkStealingThreadPool.h"
#include <unordered_map>
#include <utility>
#include <vector>

namespace GameEngine
{
namespace
{

using Editor::DecodeImage;
using Editor::HashPathAndWriteTime;
using Editor::MakeTransparent;
using Editor::RemoveStaleCacheFiles;
using Editor::ResolveAssetPathByGuid;
using Editor::ThumbnailImage;
using Editor::WritePng;
using Editor::WriteTimeOrDefault;

// Thumbnails are shown at up to 128 logical px in the asset grid; 256 keeps a
// high-DPI cell crisp without making the cache heavy.
constexpr int32 kMaxEdgePx = 256;

constexpr const char* kCachePrefix = "tilethumb_";

bool IsTileSetPath(const std::filesystem::path& path)
{
    return ToLowerAscii(path.extension().string()) == ".tileset";
}

/**
 * Names the crop a cache file belongs to: "tilethumb_<assetHash>_", then
 * "M_" for the whole map or "L<layerId>_" for a single-layer preview.
 *
 * The layer is part of the identity, not just of the content stamp: one map
 * legitimately has a live cache file per layer alongside the whole-map one, and
 * a sweep keyed on the asset alone would delete the previews its sibling rows
 * are showing. Every variant therefore ends in a terminal segment, so no
 * variant's prefix is a prefix of another's — "tilethumb_<hash>_" alone would
 * match "tilethumb_<hash>_L3_..." and make a whole-map regeneration sweep away
 * every live per-layer thumbnail.
 */
std::string CacheIdentityPrefix(const std::filesystem::path& assetPath, uint32 layerId)
{
    std::string prefix = std::string(kCachePrefix) +
                         std::to_string(std::hash<std::string>{}(assetPath.generic_string())) + "_";
    prefix += layerId != 0 ? ("L" + std::to_string(layerId) + "_") : std::string("M_");
    return prefix;
}

/** The cell's orientation bits as a blit direction. */
Editor::TileBlitOrientation CellOrientation(TileMap::TileCell cell)
{
    return Editor::TileBlitOrientation{TileMap::CellFlipH(cell), TileMap::CellFlipV(cell),
                                       TileMap::CellTranspose(cell)};
}

/** The sliced atlas, tile by tile, so the result is the grid the tileset defines. */
ThumbnailImage CompositeTileSet(const TileMap::TileSetAsset& tileSet, const ThumbnailImage& atlas)
{
    const Editor::TileMapThumbnailLayout layout = Editor::ComputeTileSetThumbnailLayout(
        tileSet.Columns(), tileSet.Rows(), tileSet.TileWidthPx(), tileSet.TileHeightPx(),
        kMaxEdgePx);
    if (!layout.Valid || atlas.IsEmpty())
        return {};

    ThumbnailImage target = MakeTransparent(layout.WidthPx, layout.HeightPx);
    for (const TileMap::TileSetTile& tile : tileSet.Tiles())
    {
        if (tile.Id == 0)
            continue;
        // Grid slices assign ids row-major and 1-based.
        const int32 index = static_cast<int32>(tile.Id) - 1;
        const int32 column = index % tileSet.Columns();
        const int32 row = index / tileSet.Columns();
        if (row >= tileSet.Rows())
            continue;
        Editor::BlitTileNearest(atlas, tile.UV, target, column * layout.CellWidthPx,
                                row * layout.CellHeightPx, layout.CellWidthPx,
                                layout.CellHeightPx);
    }
    return target;
}

/** Every layer's painted cells, bottom layer first, over transparency. */
ThumbnailImage CompositeTileMap(const TileMap::TileMapData& map, AssetManager* assetManager,
                                const std::filesystem::path& assetsRoot, uint32 onlyLayerId)
{
    // 0 draws the whole map; a layer id draws that layer alone, which is what a
    // per-layer row preview needs. Both loops below must agree on the set, or
    // the tile aspect would come from a layer the composite never draws.
    const auto participates = [onlyLayerId](const TileMap::TileMapLayerData& layer)
    {
        return layer.Type == TileMap::TileMapLayerType::Tiles &&
               (onlyLayerId == 0 || layer.Id == onlyLayerId);
    };

    // Resolve each layer's tileset once: a map's layers usually share one, and
    // decoding the same atlas per layer would dominate the whole composite.
    std::unordered_map<GUID, std::shared_ptr<TileMap::TileSetAsset>> tileSets;
    std::unordered_map<GUID, std::shared_ptr<ThumbnailImage>> atlases;
    float32 tileAspect = 1.0f;
    bool haveAspect = false;

    auto resolveTileSet = [&](const GUID& guid) -> std::shared_ptr<TileMap::TileSetAsset> {
        const auto cached = tileSets.find(guid);
        if (cached != tileSets.end())
            return cached->second;

        std::shared_ptr<TileMap::TileSetAsset> loaded;
        const std::filesystem::path path = ResolveAssetPathByGuid(assetManager, guid, assetsRoot);
        if (!path.empty())
        {
            auto candidate = std::make_shared<TileMap::TileSetAsset>(guid, path);
            if (candidate->Load())
                loaded = std::move(candidate);
        }
        tileSets.emplace(guid, loaded);
        return loaded;
    };

    auto resolveAtlas = [&](const TileMap::TileSetAsset& tileSet) -> std::shared_ptr<ThumbnailImage> {
        const GUID textureGuid = tileSet.AtlasTexture();
        const auto cached = atlases.find(textureGuid);
        if (cached != atlases.end())
            return cached->second;

        auto image = std::make_shared<ThumbnailImage>(
            DecodeImage(ResolveAssetPathByGuid(assetManager, textureGuid, assetsRoot)));
        atlases.emplace(textureGuid, image);
        return image;
    };

    for (const TileMap::TileMapLayerData& layer : map.Layers())
    {
        if (!participates(layer) || haveAspect)
            continue;
        if (const auto tileSet = resolveTileSet(layer.TileSetGuid))
        {
            if (tileSet->TileWidthPx() > 0 && tileSet->TileHeightPx() > 0)
            {
                tileAspect = static_cast<float32>(tileSet->TileWidthPx()) /
                             static_cast<float32>(tileSet->TileHeightPx());
                haveAspect = true;
            }
        }
    }

    const Editor::TileMapThumbnailLayout layout =
        Editor::ComputeTileMapThumbnailLayout(map, tileAspect, kMaxEdgePx, onlyLayerId);
    if (!layout.Valid)
        return {};

    ThumbnailImage target = MakeTransparent(layout.WidthPx, layout.HeightPx);
    bool drewAnything = false;

    for (const TileMap::TileMapLayerData& layer : map.Layers())
    {
        if (!participates(layer))
            continue;
        const auto tileSet = resolveTileSet(layer.TileSetGuid);
        if (!tileSet)
            continue;
        const auto atlas = resolveAtlas(*tileSet);
        if (!atlas || atlas->IsEmpty())
            continue;

        for (const TileMap::ChunkCoord coord : layer.SortedChunkCoords())
        {
            const TileMap::TileMapChunk* chunk = layer.FindChunk(coord);
            if (chunk == nullptr)
                continue;
            for (int32 y = 0; y < TileMap::kChunkSize; ++y)
            {
                for (int32 x = 0; x < TileMap::kChunkSize; ++x)
                {
                    const TileMap::TileCell cell =
                        chunk->Cells[static_cast<size_t>(y) * TileMap::kChunkSize + x];
                    if (TileMap::CellIsEmpty(cell))
                        continue;
                    const TileMap::TileSetTile* tile = tileSet->FindTile(TileMap::CellTileId(cell));
                    if (tile == nullptr)
                        continue;

                    const int32 cellX = coord.X * TileMap::kChunkSize + x;
                    const int32 cellY = coord.Y * TileMap::kChunkSize + y;
                    // Cell Y grows upward, image Y downward.
                    const int32 dstX = (cellX - layout.MinCellX) * layout.CellWidthPx;
                    const int32 dstY = (layout.MaxCellY - cellY) * layout.CellHeightPx;
                    Editor::BlitTileNearest(*atlas, tile->UV, target, dstX, dstY,
                                            layout.CellWidthPx, layout.CellHeightPx,
                                            CellOrientation(cell));
                    drewAnything = true;
                }
            }
        }
    }

    return drewAnything ? target : ThumbnailImage{};
}

} // namespace

TileThumbnailHandler::TileThumbnailHandler(JobSystem::WorkStealingThreadPool& jobSystem,
                                               AssetManager* assetManager)
    : m_JobSystem(jobSystem), m_AssetManager(assetManager)
{
}

void TileThumbnailHandler::ResetForProjectSwitch()
{
    {
        std::lock_guard<std::mutex> lock(m_DependencyMutex);
        m_Dependencies.clear();
    }
    // A running composite holds its own copy of the paths it was given and
    // finishes against the old cache root; dropping the entries here means its
    // result is never claimed by a request from the next project.
    std::lock_guard<std::mutex> lock(m_State->Mutex);
    m_State->InFlight.clear();
    m_State->Failed.clear();
}

std::vector<std::filesystem::path> TileThumbnailHandler::DependenciesOf(
    const std::filesystem::path& assetPath) const
{
    const std::string key = assetPath.generic_string();
    const std::filesystem::file_time_type sourceTime = WriteTimeOrDefault(assetPath);
    {
        std::lock_guard<std::mutex> lock(m_DependencyMutex);
        const auto it = m_Dependencies.find(key);
        if (it != m_Dependencies.end() && it->second.SourceWriteTime == sourceTime)
            return it->second.Dependencies;
    }

    std::vector<std::filesystem::path> dependencies;
    if (IsTileSetPath(assetPath))
    {
        TileMap::TileSetAsset tileSet(GUID::Null(), assetPath);
        if (tileSet.Load())
        {
            dependencies.push_back(
                ResolveAssetPathByGuid(m_AssetManager, tileSet.AtlasTexture(), m_AssetsRoot));
        }
    }
    else
    {
        TileMap::TileMapAsset map(GUID::Null(), assetPath);
        if (map.Load())
        {
            for (const TileMap::TileMapLayerData& layer : map.Data().Layers())
            {
                const std::filesystem::path tileSetPath =
                    ResolveAssetPathByGuid(m_AssetManager, layer.TileSetGuid, m_AssetsRoot);
                dependencies.push_back(tileSetPath);
                if (tileSetPath.empty())
                    continue;
                TileMap::TileSetAsset tileSet(layer.TileSetGuid, tileSetPath);
                if (tileSet.Load())
                {
                    dependencies.push_back(
                        ResolveAssetPathByGuid(m_AssetManager, tileSet.AtlasTexture(), m_AssetsRoot));
                }
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_DependencyMutex);
        m_Dependencies[key] = DependencyEntry{sourceTime, dependencies};
    }
    return dependencies;
}

std::filesystem::path TileThumbnailHandler::CachePathFor(const std::filesystem::path& assetPath,
                                                         uint32_t layerId) const
{
    std::error_code ec;
    if (!std::filesystem::exists(assetPath, ec))
        return {};

    // The stamp covers the asset and what it draws with, so re-exporting an
    // atlas or re-slicing a tileset invalidates the maps that use it too.
    std::size_t stamp = 0;
    HashPathAndWriteTime(stamp, assetPath);
    for (const std::filesystem::path& dependency : DependenciesOf(assetPath))
        HashPathAndWriteTime(stamp, dependency);

    // The format version sits in the name rather than inside the stamp: it says
    // on the file which pipeline drew it, and it still trails the sweep prefix,
    // so bumping it leaves the older files sweepable rather than orphaned.
    const std::string fileName = CacheIdentityPrefix(assetPath, layerId) + "v" +
                                 std::to_string(kTileThumbnailFormatVersion) + "_" +
                                 std::to_string(stamp) + ".png";
    if (!m_CacheRoot.empty())
        return m_CacheRoot / fileName;
    return assetPath.parent_path() / ("." + fileName);
}

std::string TileThumbnailHandler::GetOrRequest(const std::filesystem::path& assetPath,
                                               int /*desiredSize*/,
                                               std::function<void(const std::string& relPath)> onReady,
                                               bool /*StaticModelListThumbnail*/)
{
    return GetOrRequestVariant(assetPath, 0, std::move(onReady));
}

std::string TileThumbnailHandler::GetOrRequestLayer(
    const std::filesystem::path& assetPath, uint32_t layerId,
    std::function<void(const std::string& relPath)> onReady)
{
    return GetOrRequestVariant(assetPath, layerId, std::move(onReady));
}

std::string TileThumbnailHandler::GetOrRequestVariant(
    const std::filesystem::path& assetPath, uint32_t layerId,
    std::function<void(const std::string& relPath)> onReady)
{
    const std::filesystem::path cachePath = CachePathFor(assetPath, layerId);
    if (cachePath.empty())
    {
        if (onReady)
            onReady(std::string());
        return {};
    }

    std::error_code ec;
    if (std::filesystem::exists(cachePath, ec))
    {
        const std::string result = cachePath.generic_string();
        if (onReady)
            onReady(result);
        return result;
    }

    const std::string key = cachePath.generic_string();
    {
        std::lock_guard<std::mutex> lock(m_State->Mutex);
        if (m_State->Failed.count(key) != 0)
        {
            if (onReady)
                onReady(std::string());
            return {};
        }
        if (!m_State->InFlight.insert(key).second)
            return {};
    }

    // Decoding an atlas and compositing a map is far too much for the UI thread,
    // and the asset grid asks for every visible tile at once.
    const bool isTileSet = IsTileSetPath(assetPath);
    AssetManager* assetManager = m_AssetManager;
    const std::filesystem::path assetsRoot = m_AssetsRoot;
    const std::filesystem::path cacheRoot = m_CacheRoot;
    const std::string identityPrefix = CacheIdentityPrefix(assetPath, layerId);
    m_JobSystem.EnqueueWork([state = m_State, assetPath, cachePath, key, isTileSet, layerId, assetManager,
                 assetsRoot, cacheRoot, identityPrefix, onReady]() {
        ThumbnailImage image;
        if (isTileSet)
        {
            TileMap::TileSetAsset tileSet(GUID::Null(), assetPath);
            if (tileSet.Load())
            {
                const ThumbnailImage atlas =
                    DecodeImage(ResolveAssetPathByGuid(assetManager, tileSet.AtlasTexture(), assetsRoot));
                image = CompositeTileSet(tileSet, atlas);
            }
        }
        else
        {
            TileMap::TileMapAsset map(GUID::Null(), assetPath);
            if (map.Load())
                image = CompositeTileMap(map.Data(), assetManager, assetsRoot, layerId);
        }

        const bool wrote = WritePng(cachePath, image);
        if (wrote)
            RemoveStaleCacheFiles(cacheRoot, identityPrefix, cachePath);
        else
        {
            // An image-collection tileset, a map whose tilesets are missing, or
            // an atlas that failed to decode: report nothing and let the service
            // fall back to the same icon every other unpreviewable asset gets.
            Logger::Log::Info("TileThumbnailHandler: no preview for '{}'", assetPath.string());
        }

        {
            std::lock_guard<std::mutex> lock(state->Mutex);
            state->InFlight.erase(key);
            if (!wrote)
                state->Failed.insert(key);
        }

        if (onReady)
            onReady(wrote ? cachePath.generic_string() : std::string());
    }, JobSystem::JobPriority::Background);

    return {};
}

} // namespace GameEngine
