#pragma once

#include "ForestPlacementRuntime.h"
#include "Engine/Rendering/IRenderFeature.h"
#include "Engine/Rendering/Impostors/ImpostorStage.h"

#include <memory>
#include <optional>

namespace GameEngine { class AssetManager; }

namespace GameEngine::Engine::Renderer
{

struct DepthEmitContext;
struct ForwardEmitContext;
class Material;
struct ForestPlacementFeatureTestAccess;
struct ForestSplatCells;
struct ForestPlacementSession;
struct ImpostorBakePart;

struct ForestImpostorCard
{
    GUID Material;
    Mathematics::Vector4 Bounds;
    ImpostorFoliageSway Sway;
    float SwitchCoverage = 0.0f;
};

/// The farthest point of the card from the tree root at unit scale: its corner and its largest sway.
float ForestCardReach(const ForestImpostorCard& card);

/// Owns placement sessions through render graph submission and device recovery.
/// Thread contract: Attach and the frame members run on the main thread. ClearCards, WantsCard and
/// PublishCard run in the exclusive update of the MeshImpostor system, never while a frame declares.
class ForestPlacementFeature final : public IRenderFeature
{
public:
    /// Frees the GPU objects of every session that a stand still holds; the device outlives the feature.
    ~ForestPlacementFeature() override;

    /// Starts a session that draws the trees of `model` from `placements` within `detailDistance` meters.
    /// With `splats`, the trees beyond the near meshes draw as animated splats that fade into the overview
    /// over kSplatHandoffWidth meters past the detail distance; else they draw as impostor cards.
    /// The scene's stand producer owns the returned session; the feature keeps a weak reference.
    /// Null when the archive has no detail slots, the distance is not positive or the model is empty.
    [[nodiscard]] std::shared_ptr<ForestPlacementSession> Attach(std::shared_ptr<ForestPlacementRuntime> placements,
        AssetManager& assets, uint64 world, const GUID& model, float detailDistance, uint32 nearBudget = 0,
        std::shared_ptr<const ForestSplatCells> splats = {});

    /// Meters past the detail distance over which splats fade into the overview.
    static constexpr float kSplatHandoffWidth = 64.0f;

    /// True when a session of the world draws the model's cards near the camera.
    bool WantsCard(uint64 world, const GUID& model) const;
    bool WantsPageOcclusion(uint64 world) const;
    /// Forgets the cards of the world's sessions; the MeshImpostor system publishes them again each update.
    void ClearCards(uint64 world);
    /// Gives the model's card, and the parts its near meshes draw, to the world's sessions that want it.
    /// A card that reaches past a session's archive bound radius refuses that session for good: it
    /// logs one warning, frees its GPU objects, draws the overview only and streams no page.
    void PublishCard(uint64 world, const GUID& model, const ForestImpostorCard& card,
                     std::span<const ImpostorBakePart> sources = {});
    /// Declares the session's culled cards or splats and near meshes into one Game or Scene View per frame.
    /// A drawing Game view comes first while the world simulates, else a drawing Scene View. Returns true
    /// when standRanges and standFades together hold every visible cell that the packed stream does not
    /// draw in full; standFades holds the cells of the splat handoff band. The stand draw producer calls it.
    bool EmitDetails(ForestPlacementSession& session, const ForwardEmitContext& context,
                     const Rendering::CameraData& camera, ForestPlacementView view,
                     std::vector<DrawSubCommand>& standRanges, std::vector<ForestStandFade>& standFades);
    /// Plans the session's frame for the sun cascades of the context's view: residency for `view`, the
    /// camera's view, and for shadowPages (ForestPlacementRuntime::GatherShadowPages). The view
    /// arbitration of EmitDetails applies. True when Plan holds this frame's draws for the view.
    bool PlanShadowFrame(ForestPlacementSession& session, const DepthEmitContext& context,
                         ForestPlacementView view, std::span<const ForestShadowPage> shadowPages);

    /// True when the cascade's resident pages emitted their cards and any near mesh replacements.
    bool EmitCascadeCasters(ForestPlacementSession& session, const DepthEmitContext& context, uint32 cascade,
                            const Material& material, ForestPlacementView view);

    /// Fences the frame's detail draws and keeps the near-mesh owners it drew.
    void OnFrameSubmittedRG(Rendering::RenderGraph::RGFrame& frame,
                            const Rendering::IDevice::GpuSyncToken& token) override;
    /// Abandons the frame's uploads and drops the near-mesh owners it planned.
    void OnFrameAbortedRG(Rendering::RenderGraph::RGFrame& frame) override;
    /// Resets the sessions that declared into the retired frame stream.
    void OnFrameStreamRetiredRG(Rendering::RenderGraph::RGFrame& frame) override;
    /// Forgets the GPU objects of every session; the rebuild freed them.
    void OnDeviceRebuilt(Rendering::IDevice* device) override;

private:
    friend struct ForestPlacementFeatureTestAccess;

    void Finish(Rendering::RenderGraph::RGFrame& frame, bool submitted);
    std::vector<std::weak_ptr<ForestPlacementSession>> m_Sessions;
    uint64 m_NextSession = 0;
};

} // namespace GameEngine::Engine::Renderer
