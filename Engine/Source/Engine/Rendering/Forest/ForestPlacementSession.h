#pragma once

#include "ForestNearMeshCandidates.h"
#include "ForestPlacementCull.h"
#include "ForestPlacementGpuData.h"
#include "ForestPlacementFeature.h"
#include "ForestPlacementRenderState.h"
#include "Engine/Rendering/Impostors/ImpostorBaker.h"
#include "Engine/Rendering/ViewReadbackUtils.h"
#include "Rendering/Core/RenderGraph/RGFrameStamp.h"

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace GameEngine::Engine::Renderer
{

// The last placement count readback of a session (GE_FOREST_STATS).
struct ForestPlacementCountSample
{
    std::shared_ptr<Rendering::RGBufferReadbackTicket> Ticket;
    uint64 Frame = 0;
    Rendering::ViewId View{};
    uint32 StandPages = 0;
    uint32 DetailPages = 0;
    uint32 DetailPlacements = 0;
    uint32 Meshes = 0;
    uint32 MeshBatches = 0;
};

/// One stand's detail state: its card, its slot buffer, its page stream and its near-mesh owners.
struct ForestPlacementSession
{
    ForestPlacementCountSample CountSample;
    std::shared_ptr<ForestPlacementRuntime> Placements;
    AssetManager* Assets = nullptr;
    float DetailDistance = 0;
    uint32 NearBudget = 0;
    uint32 ShadowMeshBudget = 0;
    std::vector<ForestNearMeshCandidate> NearDemand;
    std::vector<ForestMeshOwnedPlacement> NearOwners;
    std::vector<ForestMeshOwnedPlacement> PendingNearOwners;
    Rendering::RenderGraph::RGFrameStamp PendingNearFrame;
    Rendering::ViewId NearOwnerView{};
    Rendering::ViewId PendingNearView{};
    std::vector<ImpostorBakePart> SourceParts;
    uint64 World = 0;
    GUID Model;
    std::optional<ForestImpostorCard> Card;
    bool WantsCard = false;
    std::string Scope;
    // The scope of each cascade's cull: Scope.Shadow<c>.
    std::array<std::string, kMaxDepthEmitCascades> ShadowScopes;
    Rendering::IDevice* Device = nullptr;
    Rendering::RenderGraph::RGFrame* Graph = nullptr;
    Rendering::BufferHandle Slots;
    Rendering::BufferHandle Indices;
    ForestPlacementCull Cull;
    // One cull for each sun cascade: each keeps its own visible and argument buffers.
    std::array<ForestPlacementCull, kMaxDepthEmitCascades> ShadowCulls;
    std::optional<ForestPlacementRenderState> Stream;
    ForestPlacementFrame Plan;
    std::vector<ForestPlacementGpuPage> Pages;
    std::vector<ForestDetailDraw> CascadeDraws;
    // The frame that the session declared into, until that frame submits or aborts.
    Rendering::RenderGraph::RGFrameStamp Declared;
    // The frame and the view that Plan is for. The first emit of a frame plans; the later emits
    // of the same view reuse the plan.
    Rendering::RenderGraph::RGFrameStamp Planned;
    Rendering::ViewId PlannedView{};
    bool PlanReady = false;
    // The color and the cascade draws of Plan are declared. Each is declared once.
    bool ColorDeclared = false;
    bool ShadowsDeclared = false;
    // From the overview MeshRenderer's shadowCasting: the stand casts sun shadows, and it draws in color.
    bool CastsShadows = true;
    bool DrawsColor = true;
    bool ShaderRequested = false;
    bool ShaderAvailable = false;
    bool EmittedDetails = false;
    bool EmittedNearMeshes = false;
    bool EmittedShadows = false;
    bool EmittedNearShadows = false;
    // The card reaches past the archive's bound radius: the session draws the overview only and streams nothing.
    // A reset keeps it, because the card and the archive stay the same.
    bool Refused = false;
    // The last app frames (FrameOrchestrator::WorldFrameEpoch) in which a Game view and a Scene View
    // asked for the details. The view that comes second yields to the other view.
    std::optional<uint64> GameViewFrame;
    std::optional<uint64> SceneViewFrame;

    ~ForestPlacementSession();

    /// Frees the slot and index buffers (unless the rebuild freed them) and forgets the stream and the card.
    void Reset(bool deviceRebuilt);
    /// Creates the cull pipeline, the slot and index buffers and the stream. False while one is missing.
    bool EnsureGpu(Rendering::IDevice& device);
};

} // namespace GameEngine::Engine::Renderer
