// ModelThumbnailHandler, part: Render lanes: which slot each lane renders next, the dispatch, the
// finalize of a finished render and the slot texture sizes.

#include "Thumbnails/ModelThumbnail/ModelThumbnailHandler.h"

#include "Assets/AnimationClip.h"
#include "Assets/AssetManager.h"
#include "Assets/LensFlareDefinitionAsset.h"
#include "Assets/ModelAsset.h"
#include "ECS/Entity.h"
#include "Engine/Rendering/RenderServices.h"
#include "Logger/Logger.h"
#include "Mathematics/MatrixOps.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/GPUScene.h"
#include "Thumbnails/ModelThumbnail/ModelThumbnailHandlerShared.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <memory>

namespace GameEngine
{

// GE_THUMBNAIL_CACHE_FOCUS_BEGIN
size_t ModelThumbnailHandler::FindFocusedSlot(const WindowState& ws)
{
    const auto slotFor = [](const std::unordered_map<GUID, size_t>& slotMap, const GUID& guid)
    {
        const auto it = slotMap.find(guid);
        return it == slotMap.end() ? kMaxResidentSlots : it->second;
    };
    if (!s_MaterialOrbitFocusGuid.IsNull())
        return slotFor(s_MaterialOrbitFocusIblEnabled ? ws.guidToMaterialSlot
                                                      : ws.guidToMaterialNoIblSlot,
                       s_MaterialOrbitFocusGuid);
    if (!s_LensFlareFocusGuid.IsNull())
        return slotFor(ws.guidToLensFlareSlot, s_LensFlareFocusGuid);
    if (!s_OrbitFocusGuid.IsNull())
        return slotFor(ws.guidToSlot, s_OrbitFocusGuid);
    return kMaxResidentSlots;
}
// GE_THUMBNAIL_CACHE_FOCUS_END

void ModelThumbnailHandler::DispatchPreviewLane(WindowState& ws, RenderArm arm,
                                                bool shouldAdvanceAngle)
{
    RenderLane& lane = ws.lanes[kPreviewLane];
    if (!ws.previewView || !ws.previewView->IsAlive() || lane.inFlightSlot != kMaxResidentSlots)
        return;
    const size_t slotIdx = FindFocusedSlot(ws);
    if (slotIdx >= ws.slots.size())
        return;
    Slot& slot = ws.slots[slotIdx];
    if (!slot.occupied)
        return;

    // Material orbit re-renders wait until the material's async textures are
    // bound; a textureless re-render would clobber the eventually-bound frame.
    // Models also re-render while their animation plays, and during the
    // settle frames of a fresh spawn.
    bool animates = shouldAdvanceAngle;
    bool settling = false;
    if (slot.isMaterial)
    {
        animates = animates && m_RenderServices &&
                   m_RenderServices->Textures().IsMaterialTextureBindingComplete(slot.guid);
        settling = lane.spawnedSlot == slotIdx &&
                   ThumbnailCachePolicy::IsSpawnSettling(slot.guid, lane.spawnedMaterialGuid,
                                                         lane.spawnedSettleFrames);
    }
    else if (!slot.isLensFlare)
    {
        animates = animates || (slot.animClip && slot.animClip->IsLoaded() &&
                                slot.animClip->GetDuration() > 0.0f);
        const uint32_t maxRetries = IsSpawnedModelGpuReady(lane)
                                        ? ModelThumbnailShared::kThumbnailTextureReadyMaxRetries
                                        : ModelThumbnailShared::kThumbnailGpuReadyMaxRetries;
        settling = lane.spawnedSlot == slotIdx && slot.gpuNotReadyRetries < maxRetries &&
                   ThumbnailCachePolicy::IsSpawnSettling(slot.guid, lane.spawnedModelGuid,
                                                         lane.spawnedSettleFrames);
    }
    // A resized staging view renders again at its new size.
    const Engine::Renderer::StagingView& view = *ws.previewView;
    const bool shown = ws.previewShownSlot == slotIdx && ws.previewShownGuid == slot.guid &&
                       view.OutputWidth() == view.RenderWidth() && view.OutputHeight() == view.RenderHeight();
    if (shown && !slot.needsOrbitRerender && !animates && !settling)
        return;

    // RunAnimationClipManagement advances the clip only on orbit re-renders.
    if (shown && animates)
        slot.needsOrbitRerender = true;
    DispatchSlotRender(ws, lane, arm, slot.guid, slotIdx);
}

void ModelThumbnailHandler::DispatchQueueLanes(WindowState& ws, RenderArm arm)
{
    for (size_t laneIdx = kFirstQueueLane; laneIdx < kRenderLaneCount; ++laneIdx)
    {
        RenderLane& lane = ws.lanes[laneIdx];
        if (lane.inFlightSlot != kMaxResidentSlots)
            continue;

        // A spawn mid-settle must keep re-dispatching even after its pending
        // entry was consumed on its first render — the settle sticky lives in
        // DrainNextPendingSlot, so we have to reach it. Without this the LAST
        // model in a folder (whose queue empties the instant it first renders)
        // never gets its warmup frames and bakes blank at settle=1.
        const bool settleInProgress = ThumbnailCachePolicy::IsSpawnSettling(
            lane.SpawnedGuid(), lane.SpawnedGuid(), lane.spawnedSettleFrames);
        if (ws.pending.empty() && !settleInProgress)
            continue;

        GUID guid{};
        size_t slotIdx = kMaxResidentSlots;
        if (!DrainNextPendingSlot(ws, lane, guid, slotIdx))
            continue;

        DispatchSlotRender(ws, lane, arm, guid, slotIdx);
    }
}

void ModelThumbnailHandler::DispatchSlotRender(WindowState& ws, RenderLane& lane, RenderArm arm,
                                               const GUID& guid, size_t slotIdx)
{
    if (slotIdx >= ws.slots.size() || !m_RenderServices)
        return;
    if (!arm.frame)
        return;
    Slot& slot = ws.slots[slotIdx];
    // The preview lane writes the staging view, never the slot texture a queue lane may be writing.
    if (slot.inFlight && !lane.isPreviewLane)
        return;

    // Ensure we have a device and GPUScene to back the world debug pipeline.
    Rendering::IDevice* device = m_RenderServices->GetDevice();
    Rendering::GPUScene* scene = m_RenderServices->GetGPUScene();
    if (!device || !scene)
        return;

    // Editor invariant: thumbnails and UI run on one shared device.
    if (arm.frame->Device() != device)
    {
        Logger::Log::Error("ModelThumbnailHandler: graph device mismatch under shared-device editor invariant");
        assert(false && "ModelThumbnailHandler: graph device mismatch under shared-device editor invariant");
        return;
    }

    // Slot target creation, deferred from acquisition (graph-free request
    // contract). Must precede the framing logic below — it reads texWidth.
    if (!lane.isPreviewLane)
    {
        DropBakedImage(ws, slot);
        EnsureSlotDeviceTexture(slot, slotIdx);
    }
    // The preview frames its content to the staging view's aspect; tiles are square.
    const float previewAspect = lane.isPreviewLane
        ? static_cast<float>(ws.previewView->RenderWidth()) / static_cast<float>(ws.previewView->RenderHeight())
        : 1.0f;

    using GameEngine::Mathematics::Matrix4x4;
    using GameEngine::Mathematics::Vector3;

    const bool isFocusedSlotForOrbit = lane.isPreviewLane && (slot.isMaterial
        ? (!s_MaterialOrbitFocusGuid.IsNull() && slot.guid == s_MaterialOrbitFocusGuid &&
           slot.previewIblEnabled == s_MaterialOrbitFocusIblEnabled)
        : slot.isLensFlare
            ? (!s_LensFlareFocusGuid.IsNull() && slot.guid == s_LensFlareFocusGuid)
            : (!s_OrbitFocusGuid.IsNull() && slot.guid == s_OrbitFocusGuid));
    float orbitYaw = (slot.listStaticThumb || !isFocusedSlotForOrbit) ? 0.0f : s_OrbitAngleY;

    // ---- Lens-flare preview path ----
    if (slot.isLensFlare)
    {
        SharedPtr<Asset> asset = m_AssetManager ? m_AssetManager->GetAsset(guid) : nullptr;
        auto* flareAsset = asset ? dynamic_cast<LensFlareDefinitionAsset*>(asset.get()) : nullptr;
        if (!flareAsset || !flareAsset->IsLoaded())
        {
            if (m_AssetManager && !m_AssetManager->IsLoadSuppressed(guid))
                m_AssetManager->LoadAsset(
                    guid, AssetLoadResultCallback{}, AssetLoadPriority::High);
            EnqueuePending(ws, guid, false, false, std::nullopt, true);
            return;
        }

        SpawnOrUpdateLensFlareEntity(lane, guid, orbitYaw);
        if (!lane.world || !lane.lensFlareEntity.IsValid())
            return;

        EnsureViewAndCamera(lane);
        m_RenderServices->Views().RequestViewFrame(lane.viewId);
        m_RenderServices->Views().SetViewWorldId(
            lane.viewId, lane.world->GetWorldId());

        SubmitThumbnailRenderRG(ws, lane, slot, slotIdx, guid, *arm.frame);
        return;
    }

    // ---- Material preview path ----
    if (slot.isMaterial)
    {
        if (!EnsureMaterialReady(ws, guid, slot))
        {
            // The spawn settles again once the material is ready again.
            if (lane.isSpawnedMaterial && lane.spawnedMaterialGuid == guid)
            {
                lane.spawnedSettleFrames = 0;
                lane.spawnedSlot = kMaxResidentSlots;
            }
            return;
        }

        // Primitives are unit-scale; compute a fixed framing scale per shape.
        // The preview camera has a vertical field of view, so a shape covers
        // the same fraction of the frame's height at any width: a wide frame
        // keeps the shape's size, and a tall frame shrinks it by the aspect
        // so it stays inside the width.
        const bool renderingAtPreviewSize = lane.isPreviewLane;
        float framingMul = 1.0f;
        if (renderingAtPreviewSize)
            framingMul = std::min(previewAspect, 1.0f) * s_PreviewZoomScale;

        float shapeScale = 1.05f;
        switch (s_MaterialPreviewShape)
        {
            case PreviewShape::Sphere:  shapeScale = renderingAtPreviewSize ? 1.16f : 1.05f; break;
            case PreviewShape::Cube:    shapeScale = 0.95f; break;
            case PreviewShape::Plane:   shapeScale = 1.30f; break;
        }
        shapeScale *= framingMul;

        const Matrix4x4 orbitMatrix =
            GameEngine::Mathematics::MakeScale(Vector3(shapeScale, shapeScale, shapeScale)) *
            GameEngine::Mathematics::MakeRotationY(orbitYaw);

        SpawnOrUpdateMaterialEntity(lane, guid, orbitMatrix);
        if (!lane.primitiveEntity.IsValid())
        {
            Logger::Log::Warning("MaterialThumb: primitiveEntity invalid after SpawnOrUpdateMaterialEntity, guid={}", guid.ToCompactString());
            return;
        }
        lane.spawnedSlot = slotIdx;

        ECS::World& thumbWorld = *lane.world;
        UpdatePreviewLights(lane, orbitYaw);
        EnsureViewAndCamera(lane);

        m_RenderServices->Views().RequestViewFrame(lane.viewId);
        m_RenderServices->Views().SetViewWorldId(lane.viewId, thumbWorld.GetWorldId());

        TickThumbnailSystems(thumbWorld);

        SubmitThumbnailRenderRG(ws, lane, slot, slotIdx, guid, *arm.frame);
        return;
    }

    // ---- Model preview path ----

    // Ensure the asset is loaded. If not ready, requeue and return.
    std::shared_ptr<ModelAsset> modelAsset;
    if (!EnsureAssetLoaded(ws, guid, slot, modelAsset))
        return;

    ComputeFramingBounds(slot, *modelAsset);

    // Compute orbit transform (scale, rotation, center offset).
    Vector3 minV(slot.framingMin[0], slot.framingMin[1], slot.framingMin[2]);
    Vector3 maxV(slot.framingMax[0], slot.framingMax[1], slot.framingMax[2]);
    Vector3 center = (minV + maxV) * 0.5f;
    Vector3 diff = maxV - center;
    const float modelRadius = std::sqrt(diff.x * diff.x + diff.y * diff.y + diff.z * diff.z);

    constexpr float kFovRad = 45.0f * 3.1415926535f / 180.0f;
    const Vector3 eye(0.9f, 0.9f, 1.8f);
    const Vector3 target(0.0f, 0.0f, 0.0f);
    const float camDist = (eye - target).Length();
    const bool renderingAtPreviewSize = lane.isPreviewLane;
    // Bounding-sphere cone fit: R = D * sin(fov/2) is tangent to the frustum.
    // tan(fov/2) oversizes and clips near-side AABB extents — visible on
    // elongated bakes in the inspector orientation strip (square list thumb
    // packed into a 256px-tall host). Padding shrinks the model inside that
    // cone so edge pixels and border-radius don't clip the mesh.
    constexpr float kGridFramingPadding = 1.25f;
    constexpr float kPreviewFramingPadding = 1.15f;
    float framingPadding = renderingAtPreviewSize ? kPreviewFramingPadding : kGridFramingPadding;
    if (renderingAtPreviewSize && previewAspect < 1.0f)
        framingPadding /= previewAspect;
    const float zoom = (renderingAtPreviewSize && s_PreviewZoomScale > 1e-4f)
        ? s_PreviewZoomScale
        : 1.0f;
    const float desiredRadius = camDist * std::sin(kFovRad * 0.5f) / framingPadding * zoom;

    float scale = 1.0f;
    if (modelRadius > 1e-5f)
        scale = desiredRadius / modelRadius;

    const Matrix4x4 modelMatrix =
        GameEngine::Mathematics::MakeScale(Vector3(scale, scale, scale)) *
        GameEngine::Mathematics::MakeRotationY(orbitYaw) *
        GameEngine::Mathematics::MakeTranslation(Vector3(-center.x, -center.y, -center.z));

    SpawnOrUpdateModel(lane, slot, guid, *modelAsset, modelMatrix);
    if (!lane.spawnedResult.IsValid())
        return;
    lane.spawnedSlot = slotIdx;

    ECS::World& thumbWorld = *lane.world;

    RunAnimationClipManagement(lane, slot, guid, *modelAsset);
    UpdatePreviewLights(lane, orbitYaw);
    EnsureViewAndCamera(lane);

    // Arm the view for this frame and the next, so extraction matches the thumbnail world against it;
    // it lapses on its own after them.
    m_RenderServices->Views().RequestViewFrame(lane.viewId);
    m_RenderServices->Views().SetViewWorldId(lane.viewId, thumbWorld.GetWorldId());

    TickThumbnailSystems(thumbWorld);

    SubmitThumbnailRenderRG(ws, lane, slot, slotIdx, guid, *arm.frame);
}

void ModelThumbnailHandler::FinalizeInFlightRender(WindowState& ws, RenderLane& lane)
{
    if (lane.inFlightSlot == kMaxResidentSlots || lane.inFlightFrame + 1u > ws.frameCounter)
        return;

    const size_t slotIdx = lane.inFlightSlot;
    if (lane.isPreviewLane)
    {
        // The staging view shows this slot's asset; the slot texture is the grid tile's.
        ws.previewShownSlot = slotIdx;
        ws.previewShownGuid = slotIdx < ws.slots.size() ? ws.slots[slotIdx].guid : GUID{};
        lane.inFlightSlot = kMaxResidentSlots;
        lane.inFlightFrame = 0;
        m_RenderServices->GetWorldDrawBuilder().ClearView(lane.viewId);
        return;
    }
    if (slotIdx < ws.slots.size())
    {
        Slot& slot = ws.slots[slotIdx];
        slot.inFlight = false;
        slot.needsUiDetachBeforeRender = false;
        // Single buffer — the per-frame publish/device bind picks the texture
        // up once ready flips; nothing to swap.
        slot.ready = true;
    }

    lane.inFlightSlot = kMaxResidentSlots;
    lane.inFlightFrame = 0;

    // Prevent the lane's view from re-rendering every frame due to stale
    // persistent draw lists.
    if (lane.viewId != 0)
    {
        m_RenderServices->GetWorldDrawBuilder().ClearView(lane.viewId);

        // Detach the camera so the view is fully headless until the next
        // thumbnail render reassigns it.
        m_RenderServices->Views().SetViewCamera(lane.viewId, 0u);

        // Drop any published registry targets so the view stops activating.
        m_RenderServices->Views().ClearViewTargets(lane.viewId);
    }
}

void ModelThumbnailHandler::ResizeSlotTextures(WindowState& ws)
{
    const uint32_t desiredW = static_cast<uint32_t>(s_ThumbnailResolutionPx);
    const uint32_t desiredH = desiredW;
    for (size_t i = 0; i < ws.slots.size(); ++i)
    {
        Slot& s = ws.slots[i];
        if (!s.occupied || s.listStaticThumb || s.inFlight)
            continue;
        // Only slots that already have a device texture can need a resize;
        // creation happens at dispatch.
        if (!s.deviceTex.IsValid())
            continue;
        // A baked tile keeps its PNG; a tile whose bake was persisted goes back
        // to its PNG (RequestBakedImages) instead of rendering again.
        if (s.showsBakedImage || (s.ready && m_DiskCacheRequested.count(s.guid)))
            continue;

        if (s.texWidth == desiredW && s.texHeight == desiredH)
            continue;

        // Single device texture: destroy (timeline-deferred) + recreate at the
        // new dims. The next encode treats it as uninitialized.
        if (Rendering::IDevice* device = m_RenderServices->GetDevice())
        {
            device->DestroyTexture(s.deviceTex);
            s.deviceTex = {};
            s.deviceTexInitialized = false;
            s.showsBakedImage = false;
            // Store the new dims FIRST — EnsureSlotDeviceTexture consumes
            // the stored snapshot.
            s.texWidth = desiredW;
            s.texHeight = desiredH;
            EnsureSlotDeviceTexture(s, i);
        }
        s.texWidth = desiredW;
        s.texHeight = desiredH;

        // Detach the old UI binding; the fresh texture has to be bound from scratch
        // once the slot has a completed render at the new dimensions.
        if (!s.uiKey.empty() && s.uiBound)
        {
            ws.evictedUiKeys.push_back(s.uiKey);
            s.uiBound = false;
        }
        s.ready = false;
        s.inFlight = false;
        s.needsUiDetachBeforeRender = true;
        // Re-enqueue so the new-sized slot is rendered promptly. Preserve the
        // slot's request kind — re-enqueuing a material slot as a model request
        // would route it through the wrong drain path and stall the preview.
        EnqueuePending(ws, s.guid, s.listStaticThumb, s.isMaterial,
                       s.previewIblEnabled, s.isLensFlare);
    }
}

// GE_THUMBNAIL_CACHE_DRAIN_BEGIN
bool ModelThumbnailHandler::IsSettlingInAnotherLane(const WindowState& ws, const RenderLane& lane,
                                                    size_t slotIdx)
{
    for (const RenderLane& other : ws.lanes)
    {
        if (&other == &lane || other.isPreviewLane || other.spawnedSlot != slotIdx)
            continue;
        if (ThumbnailCachePolicy::IsSpawnSettling(ws.slots[slotIdx].guid, other.SpawnedGuid(),
                                                  other.spawnedSettleFrames))
            return true;
    }
    return false;
}

bool ModelThumbnailHandler::DrainNextPendingSlot(WindowState& ws, const RenderLane& lane,
                                                 GUID& outGuid, size_t& outSlotIdx)
{
    outGuid = GUID{};
    outSlotIdx = kMaxResidentSlots;

    // Single-spawn settle stickiness. A lane's world holds ONE spawned model
    // or material (lane.SpawnedGuid()); spawning a different one tears it down
    // (world->Clear()). A fresh spawn needs a few frames to render correctly —
    // a model's instance must extract and its textures must finish binding, a
    // material's first draw requests its pass variant, and the bucketer twin
    // draws from the prior frame's slot buffers (one-frame pipeline latency).
    // If a different pending request drains before then, its spawn tears the
    // in-progress one down, so every tile in a folder renders blank and gets
    // baked "ready" (the grid stays blank until each tile is focus-selected,
    // which re-renders it every frame until it settles). So keep the spawn
    // dispatched — and only it — until it has rendered for kSpawnSettleFrames
    // fully-ready frames, then advance to the next pending request. The retry
    // cap below bounds a never-ready (geometry-less) model so it can't
    // head-of-line-block. The focused asset's tile drains like any other: the
    // preview lane renders the staging view, not the tile.
    const size_t focusedSlot = FindFocusedSlot(ws);
    if (lane.spawnedSlot < ws.slots.size() &&
        ThumbnailCachePolicy::IsSpawnSettling(lane.SpawnedGuid(), lane.SpawnedGuid(),
                                              lane.spawnedSettleFrames))
    {
        const Slot& spawnSlot = ws.slots[lane.spawnedSlot];
        // Gave up waiting for a never-ready (geometry-less) model — let the
        // normal drain bake whatever rendered and advance to the next model.
        // A model whose geometry IS in and is only waiting on async texture
        // binding gets the looser bound so the sticky keeps re-dispatching
        // until its maps land (matches SubmitThumbnailRenderRG's cap pick).
        const uint32_t maxRetries = IsSpawnedModelGpuReady(lane)
                                        ? ModelThumbnailShared::kThumbnailTextureReadyMaxRetries
                                        : ModelThumbnailShared::kThumbnailGpuReadyMaxRetries;
        if (spawnSlot.occupied && !spawnSlot.inFlight && spawnSlot.isMaterial == lane.isSpawnedMaterial &&
            !spawnSlot.isLensFlare && spawnSlot.guid == lane.SpawnedGuid() &&
            spawnSlot.gpuNotReadyRetries < maxRetries)
        {
            outGuid = spawnSlot.guid;
            outSlotIdx = lane.spawnedSlot;
            return true;
        }
    }

    // The tiles asked for since the latest scroll are the screen the user
    // looks at: the first pass takes only those. Older requests (tiles scrolled
    // past) keep their order and render when nothing newer is ready.
    uint64_t newestGeneration = 0;
    for (const PendingThumbRequest& queued : ws.pending)
        newestGeneration = std::max(newestGeneration, queued.ScrollGeneration);

    for (const bool newestOnly : {true, false})
    {
        // Cycle guard: re-enqueueing pushes an entry to the back. After we've
        // popped `initialSize` entries we've seen every original entry exactly
        // once — anything still in the queue is a re-enqueue from this Drain
        // call. Cap iterations to avoid revisiting them and infinite-looping
        // when nothing is ready. No allocations (vs an unordered_set of strings).
        size_t remaining = ws.pending.size();

        while (!ws.pending.empty() && remaining > 0)
        {
            --remaining;
            const PendingThumbRequest req = ws.pending.front();
            ws.pending.pop_front();
            ws.pendingSet.erase({req.Guid, req.ListStatic, req.IsMaterial,
                                 req.IsLensFlare, req.PreviewIblEnabled});
            if (newestOnly && req.ScrollGeneration != newestGeneration)
            {
                RequeuePending(ws, req);
                continue;
            }

            auto& guidMap = req.IsLensFlare
                ? ws.guidToLensFlareSlot
                : (req.IsMaterial
                    ? (req.PreviewIblEnabled ? ws.guidToMaterialSlot : ws.guidToMaterialNoIblSlot)
                    : (req.ListStatic ? ws.guidToListSlot : ws.guidToSlot));
            auto itSlot = guidMap.find(req.Guid);
            if (itSlot == guidMap.end())
                continue;

            const size_t slotIdx = itSlot->second;
            if (slotIdx >= ws.slots.size())
                continue;

            Slot& slot = ws.slots[slotIdx];
            // The focused slot's re-render flag belongs to the preview lane: its
            // tile renders until it is ready, then stays.
            const bool rerender = slot.needsOrbitRerender && slotIdx != focusedSlot;
            if (!slot.occupied || (slot.ready && !rerender))
                continue;

            // A lane that is still settling a model re-renders it on its own; the request is spent.
            if (IsSettlingInAnotherLane(ws, lane, slotIdx))
                continue;

            // Another lane renders this slot this frame; the request still stands.
            if (slot.inFlight)
            {
                RequeuePending(ws, req);
                continue;
            }

            if (slot.needsUiDetachBeforeRender)
            {
                slot.needsUiDetachBeforeRender = false;
                RequeuePending(ws, req);
                continue;
            }

            // Skip requests whose asset is still loading or whose material
            // textures are still binding. Re-enqueue at the back and keep
            // iterating, so an idle lane renders a request that is ready this frame
            // instead of spending the frame on one that is not.
            if (req.IsMaterial && m_RenderServices &&
                !m_RenderServices->Textures().IsMaterialTextureBindingComplete(req.Guid))
            {
                RequeuePending(ws, req);
                continue;
            }
            if (!req.IsMaterial && !req.IsLensFlare)
            {
                const std::shared_ptr<ModelAsset> model = AcquireSlotModel(slot, req.Guid);
                if (IsSlotModelImporting(slot) || (model && !model->IsLoaded()))
                {
                    if (model)
                        NoteLoadWait(slot, *model, "the asset never reports loaded");
                    RequeuePending(ws, req);
                    continue;
                }
            }

            outGuid = req.Guid;
            outSlotIdx = slotIdx;
            return true;
        }
    }

    return false;
}
// GE_THUMBNAIL_CACHE_DRAIN_END

void ModelThumbnailHandler::NoteLoadWait(Slot& slot, const Asset& asset, const char* reason)
{
    const auto now = std::chrono::steady_clock::now();
    if (slot.loadWaitSince == std::chrono::steady_clock::time_point{})
        slot.loadWaitSince = now;
    if (slot.loadWaitReported || now - slot.loadWaitSince < kLoadWaitWarnAfter)
        return;
    slot.loadWaitReported = true;
    Logger::Log::Warning("ModelThumbnailHandler: thumbnail for '{}' has waited {} s because {}; "
                         "it stays queued while the queue lanes render ready requests",
                         asset.GetPath().string(), kLoadWaitWarnAfter.count(), reason);
}

} // namespace GameEngine
