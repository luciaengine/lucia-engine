#pragma once

#include "ForestNearMeshCandidates.h"
#include "ForestPlacementCull.h"
#include "ForestPlacementGpuData.h"
#include "ForestPlacementFeature.h"
#include "ForestPlacementRenderState.h"
#include "Engine/Rendering/Impostors/ImpostorBaker.h"
#include "Engine/Rendering/ViewReadbackUtils.h"
#include "Rendering/Core/RenderGraph/RGFrameStamp.h"

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
    Rendering::IDevice* Device = nullptr;
    Rendering::RenderGraph::RGFrame* Graph = nullptr;
    Rendering::BufferHandle Slots;
    Rendering::BufferHandle Indices;
    ForestPlacementCull Cull;
    std::optional<ForestPlacementRenderState> Stream;
    ForestPlacementFrame Plan;
    std::vector<ForestPlacementGpuPage> Pages;
    // The frame that the session declared into, until that frame submits or aborts.
    Rendering::RenderGraph::RGFrameStamp Declared;
    bool ShaderRequested = false;
    bool ShaderAvailable = false;
    bool EmittedDetails = false;
    bool EmittedNearMeshes = false;
    // The card reaches past the archive's bound radius: the session draws the overview only and streams nothing.
    // A reset keeps it, because the card and the archive stay the same.
    bool Refused = false;
    // The last app frame (FrameOrchestrator::WorldFrameEpoch) in which a Game view asked for the details.
    // A Scene View yields to it.
    std::optional<uint64> GameViewFrame;

    ~ForestPlacementSession();

    /// Frees the slot and index buffers (unless the rebuild freed them) and forgets the stream and the card.
    void Reset(bool deviceRebuilt);
    /// Creates the cull pipeline, the slot and index buffers and the stream. False while one is missing.
    bool EnsureGpu(Rendering::IDevice& device);
};

} // namespace GameEngine::Engine::Renderer
