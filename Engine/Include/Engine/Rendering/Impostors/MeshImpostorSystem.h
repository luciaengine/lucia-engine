#pragma once

#include "AssetCore/AssetReloadInvalidator.h"
#include "AssetCore/GUID.h"
#include "ECS/ChangeFilter.h"
#include "ECS/ECS.h"
#include "ECS/Systems.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "Mathematics/Vector3.h"
#include "Mathematics/WorldPosition.h"
#include "Types/FlatMap.h"
#include "Types/Types.h"

#include <atomic>
#include <cstddef>
#include <span>
#include <vector>

namespace GameEngine::Engine::Renderer
{

class ImpostorBaker;
class Material;
class ImpostorStage;
class ForestPlacementFeature;
class RenderServices;

/// Gives the impostor stage to every model whose setting `assets.model.impostor` is on
/// (docs/development/tree-impostor-stage.mdx, Step 4). It finds the parts the world draws of each
/// such model, bakes the model's atlas, registers its card material and links the parts to the
/// card once the card can draw. The scatter then selects mesh or card per instance on the GPU.
/// An evicted atlas unlinks the parts, so they draw their meshes again.
class MeshImpostorSystem : public ECS::ISystem
{
public:
    explicit MeshImpostorSystem(RenderServices* renderServices);
    ~MeshImpostorSystem() override;

    const char* GetName() const override { return "MeshImpostor"; }
    void Update(ECS::World& world, float32 deltaTime) override;
    // It writes GPUScene mesh rows and batch counts, which have one writer at a time.
    bool RequiresExclusiveUpdate() const override { return true; }

    /// One part of a model with the material it bakes with.
    struct Part
    {
        Rendering::MeshGPUHandle Mesh;
        GUID Material;

        bool operator==(const Part&) const = default;
    };

private:
    struct PlacementDemand
    {
        Mathematics::WorldPosition Center;
        float Radius;
        float LodBias;
        uint32 Layers;
        bool EditorHidden;
    };

    struct PlacementDemandBounds
    {
        Mathematics::WorldPosition Center;
        float CenterRadius = 0.0f;
        float MaxRadius = 0.0f;
        float MinBiasedRadius = 0.0f;
        uint32 Layers = 0;
        uint32 EditorLayers = 0;
    };

    // Preorder traversal skips a rejected node's entire subtree. Leaves hold placement ranges.
    struct PlacementDemandNode
    {
        PlacementDemandBounds Bounds;
        size_t SubtreeEnd;
        size_t First;
        size_t Count;
    };

    struct ModelImpostor
    {
        // Indexed by submesh. An empty part prevents the model from linking.
        std::vector<Part> Parts;
        Mathematics::Vector3 Center;
        float Radius = 0.0f;
        float SwitchCoverage = 0.0f;
        uint64 MeshHash = 0;
        bool Clustered = false;
        bool HasConflictingParts = false;
        std::vector<PlacementDemand> Placements;
        std::vector<PlacementDemandNode> DemandTree;
        bool WantsAtlas = false;
        // The card material index and the part mesh indices the link holds; empty while not linked.
        uint32 LinkedMaterial = 0xFFFFFFFFu;
        std::vector<uint32> LinkedMeshes;

        bool RequiresPlacement() const { return Clustered || Parts.size() > 1; }
    };

    // Rebuilds the parts of the opted-in models from the world's mesh renderers.
    void Scan(ECS::World& world);
    void UpdatePlacements(ECS::World& world);
    void RefreshAtlasDemand(uint64 world);
    static PlacementDemandBounds BuildPlacementDemandBounds(std::span<const PlacementDemand> placements,
                                                              double Mathematics::WorldPosition::*& splitAxis);
    static void BuildPlacementDemandTree(ModelImpostor& model, size_t first, size_t count);
    // Returns the card material once the parts are linked to it, else null.
    const Material* UpdateModel(const GUID& model, ModelImpostor& impostor, ImpostorBaker& baker,
                                ImpostorStage& stage, ForestPlacementFeature* forest, uint64 world);
    void UnlinkModel(ModelImpostor& impostor);

    RenderServices* m_RenderServices = nullptr;
    FlatMap<GUID, ModelImpostor> m_Models;
    ECS::ChangeGate m_MeshRendererGate;
    ECS::ChangeGate m_PlacementTransformGate;
    ECS::ChangeGate m_PlacementBoundsGate;
    ECS::ChangeGate m_PlacementLodGate;
    std::vector<ECS::EntityHandle> m_PlacementMembers;
    uint64 m_PlacementSceneEpoch = 0;
    bool m_WaitingForPlacementSlots = false;
    // A destroy that empties its chunk stamps no column, so the structural version also starts a scan.
    std::size_t m_StructuralVersion = static_cast<std::size_t>(-1);
    uint64 m_DeviceGeneration = 0;
    std::atomic<bool> m_ModelReloaded{false};
    AssetReloadInvalidator m_ModelReloadInvalidator;
};

} // namespace GameEngine::Engine::Renderer
