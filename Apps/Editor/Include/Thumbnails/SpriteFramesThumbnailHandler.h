#pragma once
#include <array>

#include "Thumbnails/AssetThumbnailHandler.h"

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
 * @brief Thumbnails for `.spriteframes`: one representative frame, cropped out
 * of the sheet it lives in.
 *
 * A sprite sheet's own texture is a useless preview — a grid of tiny poses on
 * transparency — so the thumbnail is a single frame's sub-rect blown up to fill
 * the tile. The frame is deterministic (the first of the chosen animation), not
 * animated: a hierarchy full of ticking thumbnails costs a decode per frame and
 * reads as noise.
 *
 * The Hierarchy asks per entity, so the crop is keyed by the animation name and
 * facing that entity is showing; the asset browser asks for the asset alone and
 * gets the first animation, unflipped.
 *
 * Cache files are stamped with the write times of the asset and of every texture
 * its frames reference, so re-exporting a sheet or re-slicing the frames asks
 * for a file that does not exist yet and regenerates.
 */
class SpriteFramesThumbnailHandler : public AssetThumbnailHandler
{
public:
    std::string GetOrRequestAtlasRegion(const std::filesystem::path& texturePath,
        const std::array<float, 4>& uv, std::function<void(const std::string&)> onReady,
        const std::array<float, 2>& magentaKey = {-1.0f, 0.0f});
    SpriteFramesThumbnailHandler(JobSystem::WorkStealingThreadPool& jobSystem, AssetManager* assetManager);
    ~SpriteFramesThumbnailHandler() override = default;

    void SetCacheRoot(const std::filesystem::path& root) override { m_CacheRoot = root; }

    // Drops the dependency map and the in-flight/failed sets. All three are keyed
    // by paths from the project being closed, and a sprite sheet at the same
    // relative path in the next project is a different asset.
    void ResetForProjectSwitch() override;

    std::string GetOrRequest(const std::filesystem::path& assetPath,
                             int desiredSize,
                             std::function<void(const std::string& relPath)> onReady,
                             bool StaticModelListThumbnail = false) override;

    /**
     * The first frame of \p animationName (the first animation when empty or
     * unknown), mirrored when \p flipX. Returns the cache path when it already
     * exists, otherwise empty and \p onReady fires on a worker thread.
     */
    std::string GetOrRequestFrame(const std::filesystem::path& assetPath,
                                  const std::string& animationName,
                                  bool flipX,
                                  std::function<void(const std::string& relPath)> onReady);

private:
    /** Cache file for this crop at the sources' current write times, or empty when unresolvable. */
    std::filesystem::path CachePathFor(const std::filesystem::path& assetPath,
                                       const std::string& animationName,
                                       bool flipX) const;

    /**
     * The textures this asset's frames sample, re-parsed only when the asset
     * itself changes. The cache stamp has to stat them on every lookup;
     * re-reading the JSON to rediscover them would put a file parse on the UI
     * thread for every visible row, every time the tree rebinds.
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
         * Crops that produced nothing. Without this a sprite whose sheet is
         * missing would spawn a decode thread on every row bind, and the
         * Hierarchy rebinds on every scroll. Keys are content-stamped, so
         * fixing the source asks under a new key and retries on its own.
         */
        std::unordered_set<std::string> Failed;
    };
    std::shared_ptr<InFlightState> m_State = std::make_shared<InFlightState>();
};

} // namespace GameEngine
