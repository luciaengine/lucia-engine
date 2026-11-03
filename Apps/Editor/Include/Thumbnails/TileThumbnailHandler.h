#pragma once

#include "AssetCore/Types.h"
#include "Thumbnails/AssetThumbnailHandler.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace JobSystem { class WorkStealingThreadPool; }

namespace GameEngine
{

class AssetManager;

/**
 * @brief Stamped into every tile thumbnail's cache file name.
 *
 * Cache names carry their sources' write times, which a code change does not
 * move — so a change to how cells are drawn must bump this, or already-generated
 * thumbnails keep their old pixels for as long as their inputs sit still. Public
 * because a test locks its presence in the name; nothing else should read it.
 */
inline constexpr int32 kTileThumbnailFormatVersion = 2;

/**
 * @brief Thumbnails for `.tileset` and `.tilemap`: the sliced atlas, and the
 * painted cells composited from the atlases their layers reference.
 *
 * One handler serves both types because they share everything that matters —
 * the atlas decode, the nearest-sampled blit, the cache file, and the
 * background thread. It is registered twice, once per AssetType.
 *
 * Compositing is CPU-side at thumbnail resolution and the result is a PNG in
 * the thumbnail cache: the GPU thumbnail path exists for assets that need the
 * real renderer (models, materials), and a tile grid does not.
 *
 * The cache file name carries the source's write time — the tilemap's, plus
 * every tileset it references — so editing any of them asks for a file that
 * does not exist yet and regenerates, with no invalidation hook to keep in
 * sync with the writers.
 */
class TileThumbnailHandler : public AssetThumbnailHandler
{
public:
    TileThumbnailHandler(JobSystem::WorkStealingThreadPool& jobSystem, AssetManager* assetManager);
    ~TileThumbnailHandler() override = default;

    void SetCacheRoot(const std::filesystem::path& root) override { m_CacheRoot = root; }

    // Drops the dependency map and the in-flight/failed sets. All three are keyed
    // by paths from the project being closed, and a tile set at the same relative
    // path in the next project is a different asset.
    void ResetForProjectSwitch() override;

    std::string GetOrRequest(const std::filesystem::path& assetPath,
                             int desiredSize,
                             std::function<void(const std::string& relPath)> onReady,
                             bool StaticModelListThumbnail = false) override;

    /**
     * One layer of a `.tilemap`, framed to that layer's own painted cells.
     *
     * The Hierarchy shows a row per layer entity, and compositing the whole map
     * for each of them would draw every row of a level identically — which is
     * the thing a preview is there to prevent. Returns the cache path when it
     * already exists, otherwise empty and \p onReady fires on a worker thread.
     */
    std::string GetOrRequestLayer(const std::filesystem::path& assetPath,
                                  uint32_t layerId,
                                  std::function<void(const std::string& relPath)> onReady);

private:
    /**
     * Cache file for this asset at its current content stamp, or empty when
     * unresolvable. \p layerId 0 is the whole map, matching TileMapLayerRef's
     * "0 = unset" convention.
     */
    std::filesystem::path CachePathFor(const std::filesystem::path& assetPath,
                                       uint32_t layerId = 0) const;

    /** Shared body of both request entry points. */
    std::string GetOrRequestVariant(const std::filesystem::path& assetPath,
                                    uint32_t layerId,
                                    std::function<void(const std::string& relPath)> onReady);

    /**
     * The files this asset draws with (tilesets, atlas textures), re-parsed only
     * when the asset itself changes. The cache stamp has to stat them on every
     * lookup; re-reading the JSON to rediscover them would put a file parse on
     * the UI thread for every visible asset tile, every frame it is unresolved.
     */
    std::vector<std::filesystem::path> DependenciesOf(const std::filesystem::path& assetPath) const;

    JobSystem::WorkStealingThreadPool& m_JobSystem;
    AssetManager* m_AssetManager = nullptr; // not owned
    std::filesystem::path m_CacheRoot;

    struct DependencyEntry
    {
        std::filesystem::file_time_type SourceWriteTime;
        std::vector<std::filesystem::path> Dependencies;
    };
    mutable std::mutex m_DependencyMutex;
    mutable std::unordered_map<std::string, DependencyEntry> m_Dependencies;

    // In-flight keys live behind a shared_ptr so a detached worker can clear its
    // entry without touching `this`, which may be gone by then.
    struct InFlightState
    {
        std::mutex Mutex;
        std::unordered_set<std::string> InFlight;
        /**
         * Requests that produced nothing — an empty layer, a missing tileset, an
         * atlas that would not decode. Without this a Collision or IntGrid layer
         * would start a fresh composite on every row bind, and the Hierarchy
         * rebinds on every scroll. Keys are content-stamped, so painting into
         * the layer asks under a new key and retries on its own.
         */
        std::unordered_set<std::string> Failed;
    };
    std::shared_ptr<InFlightState> m_State = std::make_shared<InFlightState>();
};

} // namespace GameEngine
