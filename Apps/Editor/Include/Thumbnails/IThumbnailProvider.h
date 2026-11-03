#pragma once
#include <array>

#include "AssetCore/GUID.h"
#include "Thumbnails/FolderBakeReport.h"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <cstdint>
#include <string>
#include <utility>

namespace GameEngine {

class IThumbnailProvider {
public:
    virtual std::string GetOrRequestSpriteFrame(const std::filesystem::path& /*path*/,
        const std::string& /*animation*/, bool /*flipX*/,
        std::function<void(const std::string&)> /*ready*/) { return {}; }
    virtual std::string GetOrRequestAtlasRegion(const std::filesystem::path& /*path*/,
        const std::array<float, 4>& /*uv*/, std::function<void(const std::string&)> /*ready*/,
        const std::array<float, 2>& /*magentaKey*/ = {-1.0f, 0.0f}) { return {}; }
    virtual ~IThumbnailProvider() = default;

    // Set the runtime Assets/ root so returned paths can be relative to it.
    virtual void SetAssetsRoot(const std::filesystem::path& root) = 0;

    // Returns a relative path (from Assets/) to a thumbnail or representative image if already cached.
    // If not cached, starts an async request and invokes onReady with the relative path when ready.
    // ThumbnailService delivers callbacks on its next UI BeginFrame. Other providers may
    // complete on a worker; callers must follow the concrete provider's threading contract.
    // When \p StaticModelListThumbnail is true (models only), use a separate cached
    // engine texture that does not share the rotating Asset View preview.
    virtual std::string GetOrRequest(const std::filesystem::path& assetPath,
                                     int desiredSize,
                                     std::function<void(const std::string& relPath)> onReady,
                                     bool StaticModelListThumbnail = false) = 0;

    // Browser cells call this on the UI thread. Implementations can defer a
    // miss; stillVisible is evaluated on the UI thread before starting work.
    virtual std::string GetOrRequestBrowser(const std::filesystem::path& assetPath,
                                            int desiredSize, const std::string& /*consumer*/,
                                            std::function<bool()> /*stillVisible*/,
                                            std::function<void(const std::string&)> onReady)
    {
        return GetOrRequest(assetPath, desiredSize, std::move(onReady));
    }

    // Returns true once the live-rendered thumbnail backing `engineName`
    // (e.g. `engine:editor_material_thumb_<guid>`) has produced a frame and
    // is currently bound for UI sampling in the given window. Default
    // implementation returns false — callers can use this to defer
    // swapping a focused preview's bound resource until the new render is
    // ready, avoiding a one-frame blank gap during transitions.
    virtual bool IsEngineThumbnailReadyForWindow(uint64_t /*windowId*/,
                                                 const std::string& /*engineName*/) const
    {
        return false;
    }

    // Ask the provider to produce a live engine thumbnail for `engineName`
    // in `windowId` even before the UI resolver observes the missing texture.
    virtual void EnsureEngineThumbnailRequested(uint64_t /*windowId*/,
                                                const std::string& /*engineName*/)
    {
    }

    // Bakes the persistent thumbnail of every asset under `folder` and its
    // subfolders in the background, after the visible tiles, so later visits
    // show them at once. Returns how many assets were queued; a provider
    // without a persistent cache queues none.
    virtual size_t GenerateFolderThumbnails(const std::filesystem::path& /*folder*/) { return 0; }

    // Called on the main thread whenever the current folder bake's report
    // changes, and once more when it finishes. One listener; an empty function
    // removes it.
    virtual void SetFolderBakeListener(std::function<void(const FolderBakeReport&)> /*listener*/) {}

    // Dedicated live material preview surface. Unlike GetOrRequest(), this is
    // not an asset-browser thumbnail lookup: callers already have a runtime
    // material GUID and want a UI-sampleable engine resource of the requested
    // size for a focused preview surface.
    virtual std::string RequestLiveMaterialPreview(uint64_t /*windowId*/,
                                                   const GUID& /*materialGuid*/,
                                                   uint32_t /*widthPx*/,
                                                   uint32_t /*heightPx*/,
                                                   bool /*previewIblEnabled*/ = true)
    {
        return {};
    }

    virtual bool IsLiveMaterialPreviewReady(uint64_t windowId,
                                            const std::string& engineName) const
    {
        return IsEngineThumbnailReadyForWindow(windowId, engineName);
    }
};

} // namespace GameEngine
