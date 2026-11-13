// Compile the actual focus lookup and lane drain of the thumbnail handler. Only the asset,
// texture and request hosts are deterministic substitutes; no copy of the drain loop lives
// here. This is not a live GPU test: the preview lane's staging view is out of scope.

#include "Thumbnails/ModelThumbnail/ModelThumbnailCachePolicy.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <cstdint>
#include <deque>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ThumbnailDrainHost
{
using GameEngine::GUID;
namespace ThumbnailCachePolicy = GameEngine::ThumbnailCachePolicy;
namespace ModelThumbnailShared
{
inline constexpr uint32_t kThumbnailGpuReadyMaxRetries = 32u;
inline constexpr uint32_t kThumbnailTextureReadyMaxRetries = 600u;
}
struct Asset {};
struct ModelAsset : Asset
{
    bool IsLoaded() const { return true; }
};
struct TextureBindings
{
    bool IsMaterialTextureBindingComplete(const GUID&) const { return true; }
};
struct RenderServices
{
    TextureBindings textures;
    TextureBindings& Textures() { return textures; }
};

struct ModelThumbnailHandler
{
    static constexpr size_t kMaxResidentSlots = 256;
    struct Slot
    {
        GUID guid;
        bool occupied = true, isMaterial = false, isLensFlare = false;
        bool ready = false, inFlight = false, needsOrbitRerender = false, needsUiDetachBeforeRender = false;
        uint32_t gpuNotReadyRetries = 0;
    };
    struct RenderLane
    {
        bool isPreviewLane = false, isSpawnedMaterial = false;
        GUID spawnedModelGuid, spawnedMaterialGuid;
        size_t spawnedSlot = kMaxResidentSlots;
        uint32_t spawnedSettleFrames = 0;
        const GUID& SpawnedGuid() const { return isSpawnedMaterial ? spawnedMaterialGuid : spawnedModelGuid; }
    };
    struct PendingThumbRequest
    {
        GUID Guid{};
        bool ListStatic = false, IsMaterial = false, IsLensFlare = false, PreviewIblEnabled = true;
        uint64_t ScrollGeneration = 0;
    };
    struct WindowState
    {
        struct PendingKey
        {
            GUID Guid;
            bool ListStatic, IsMaterial, IsLensFlare, PreviewIblEnabled;
            bool operator==(const PendingKey&) const = default;
        };
        struct PendingKeyHash
        {
            size_t operator()(const PendingKey& key) const noexcept { return std::hash<GUID>{}(key.Guid); }
        };
        std::vector<Slot> slots;
        std::vector<RenderLane> lanes;
        std::unordered_map<GUID, size_t> guidToSlot, guidToListSlot, guidToLensFlareSlot;
        std::unordered_map<GUID, size_t> guidToMaterialSlot, guidToMaterialNoIblSlot;
        std::deque<PendingThumbRequest> pending;
        std::unordered_set<PendingKey, PendingKeyHash> pendingSet;
    };

    static inline GUID s_OrbitFocusGuid{}, s_MaterialOrbitFocusGuid{}, s_LensFlareFocusGuid{};
    static inline bool s_MaterialOrbitFocusIblEnabled = true;
    RenderServices* m_RenderServices = nullptr;

    static size_t FindFocusedSlot(const WindowState& ws);
    static bool IsSettlingInAnotherLane(const WindowState& ws, const RenderLane& lane, size_t slotIdx);
    bool DrainNextPendingSlot(WindowState& ws, const RenderLane& lane, GUID& outGuid, size_t& outSlotIdx);
    bool IsSpawnedModelGpuReady(const RenderLane&) const { return true; }
    static bool IsSlotModelImporting(const Slot&) { return false; }
    std::shared_ptr<ModelAsset> AcquireSlotModel(Slot&, const GUID&) { return std::make_shared<ModelAsset>(); }
    void NoteLoadWait(Slot&, const Asset&, const char*) {}
    static void RequeuePending(WindowState& ws, const PendingThumbRequest& request)
    {
        if (ws.pendingSet.insert({request.Guid, request.ListStatic, request.IsMaterial, request.IsLensFlare,
                                  request.PreviewIblEnabled})
                .second)
            ws.pending.push_back(request);
    }
};
#include "ThumbnailCacheFocusExtracted.h"
#include "ThumbnailCacheDrainExtracted.h"

// A window with the preview lane, one queue lane and the focused model's tile in slot 0.
struct FocusedWindow
{
    ModelThumbnailHandler handler;
    ModelThumbnailHandler::WindowState window;
    GUID focused = GUID::Generate();

    FocusedWindow()
    {
        ModelThumbnailHandler::s_OrbitFocusGuid = focused;
        ModelThumbnailHandler::Slot tile;
        tile.guid = focused;
        window.slots = {tile};
        window.guidToSlot[focused] = 0;
        window.lanes.resize(2);
        window.lanes[0].isPreviewLane = true;
    }
    ~FocusedWindow() { ModelThumbnailHandler::s_OrbitFocusGuid = GUID{}; }
    void RequestTile() { ModelThumbnailHandler::RequeuePending(window, {focused}); }
    ModelThumbnailHandler::RenderLane& QueueLane() { return window.lanes[1]; }
};
}

// The preview lane renders the focused asset into its staging view, not into the tile, so
// the focused tile drains on a queue lane like any other, even while the preview lane settles it.
TEST(ThumbnailDrain, TheFocusedAssetsTileDrainsOnAQueueLane)
{
    using namespace ThumbnailDrainHost;
    FocusedWindow host;
    ModelThumbnailHandler::RenderLane& preview = host.window.lanes[0];
    preview.spawnedModelGuid = host.focused;
    preview.spawnedSlot = 0;
    host.RequestTile();

    GUID guid;
    size_t slotIdx = ModelThumbnailHandler::kMaxResidentSlots;
    ASSERT_TRUE(host.handler.DrainNextPendingSlot(host.window, host.QueueLane(), guid, slotIdx));
    EXPECT_EQ(guid, host.focused);
    EXPECT_EQ(slotIdx, 0u);
}

// A queue lane keeps the focused tile it spawned until the tile settles.
TEST(ThumbnailDrain, AQueueLaneKeepsTheFocusedTileItSpawnedUntilItSettles)
{
    using namespace ThumbnailDrainHost;
    FocusedWindow host;
    ModelThumbnailHandler::RenderLane& lane = host.QueueLane();
    lane.spawnedModelGuid = host.focused;
    lane.spawnedSlot = 0;
    lane.spawnedSettleFrames = 1;

    GUID guid;
    size_t slotIdx = ModelThumbnailHandler::kMaxResidentSlots;
    ASSERT_TRUE(host.handler.DrainNextPendingSlot(host.window, lane, guid, slotIdx));
    EXPECT_EQ(guid, host.focused);
    EXPECT_EQ(slotIdx, 0u);
}

// The orbit re-render flag belongs to the preview lane: a ready focused tile stays as it is.
TEST(ThumbnailDrain, AReadyFocusedTileIgnoresThePreviewRerenderFlag)
{
    using namespace ThumbnailDrainHost;
    FocusedWindow host;
    host.window.slots[0].ready = true;
    host.window.slots[0].needsOrbitRerender = true;
    host.RequestTile();

    GUID guid;
    size_t slotIdx = ModelThumbnailHandler::kMaxResidentSlots;
    EXPECT_FALSE(host.handler.DrainNextPendingSlot(host.window, host.QueueLane(), guid, slotIdx));
    EXPECT_TRUE(host.window.pending.empty());
}
