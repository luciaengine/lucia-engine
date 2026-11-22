#pragma once

#include "AssetCore/AssetReloadInvalidator.h"
#include "AssetCore/GUID.h"
#include "ECS/ChangeFilter.h"
#include "ECS/Systems.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "Mathematics/Vector3.h"
#include "Types/FlatMap.h"
#include "Types/FlatSet.h"
#include "Types/Types.h"

#include <atomic>
#include <cstddef>
#include <string_view>
#include <vector>

namespace GameEngine
{
class Asset;
class AssetManager;
namespace Components
{
struct MeshRenderer;
struct WorldTransform;
}
} // namespace GameEngine

namespace GameEngine::Engine::Renderer
{

class ImpostorBaker;
class Material;
class ImpostorStage;
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
    static constexpr uint32 kNoInstance = 0xFFFFFFFFu;

    // One placement of a model: the renderers of its parts with the same world transform.
    struct Placement
    {
        // The GPU instance of each part, indexed by submesh; kNoInstance while missing.
        std::vector<uint32> Instances;
        // Two renderers of one part share the transform: no part can own the decision.
        bool HasDuplicatePart = false;
    };

    struct ModelImpostor
    {
        // Indexed by submesh. An empty part prevents the model from linking.
        std::vector<Part> Parts;
        Mathematics::Vector3 Center;
        float Radius = 0.0f;
        float SwitchCoverage = 0.0f;
        uint64 MeshHash = 0;
        bool HasConflictingParts = false;
        // The card material index and the part mesh indices the link holds; empty while not linked.
        uint32 LinkedMaterial = 0xFFFFFFFFu;
        std::vector<uint32> LinkedMeshes;
        // Keyed by a hash of the world transform (PlacementKey).
        FlatMap<uint64, Placement> Placements;
    };

    // The models a scan has met: a null source records a model checked but not eligible.
    struct ScanState
    {
        AssetManager* Assets = nullptr;
        FlatMap<GUID, SharedPtr<Asset>> Sources;
        FlatMap<GUID, ModelImpostor> Models;
        // Opted-in models that cannot take a card, with the reason.
        FlatMap<GUID, std::string_view> Refusals;
        // A part had no GPU instance yet, so its placement waits for the next scan.
        bool MissingInstance = false;
    };

    // Rebuilds the parts of the opted-in models from the world's mesh renderers.
    void Scan(ECS::World& world);
    // Adds one mesh renderer to the scan: the model's eligibility on first sight, then its part and
    // its place in the placement of its world transform.
    static void AddRenderer(ScanState& scan, const Components::MeshRenderer& renderer,
                            const Components::WorldTransform& transform, uint32 instanceIndex);
    // Names the owner of every part of every complete placement on the GPU scene, and clears the
    // owners that the scan no longer finds.
    void ApplyOwners();
    // Returns the card material once the parts are linked to it, else null.
    const Material* UpdateModel(const GUID& model, ModelImpostor& impostor, ImpostorBaker& baker,
                                ImpostorStage& stage);
    void UnlinkModel(ModelImpostor& impostor);
    // Warns once that an opted-in model keeps its meshes, and why.
    void LogRefusal(const GUID& model, std::string_view reason);

    RenderServices* m_RenderServices = nullptr;
    FlatMap<GUID, ModelImpostor> m_Models;
    // The owners on the GPU scene, by instance (GPUScene::SetInstanceImpostorOwner).
    FlatMap<uint32, uint32> m_Owners;
    // A part without a GPU instance retries the scan after kRescanInterval updates, not every frame.
    static constexpr uint32 kRescanInterval = 16;
    uint32 m_UpdatesUntilRescan = 0;
    bool m_RescanPending = false;
    // The models with a refusal warning; a model leaves the set when it links.
    FlatSet<GUID> m_RefusalsLogged;
    ECS::ChangeGate m_MeshRendererGate;
    // A destroy that empties its chunk stamps no column, so the structural version also starts a scan.
    std::size_t m_StructuralVersion = static_cast<std::size_t>(-1);
    uint64 m_DeviceGeneration = 0;
    uint64 m_Frame = 0;
    std::atomic<bool> m_ModelReloaded{false};
    AssetReloadInvalidator m_ModelReloadInvalidator;
};

} // namespace GameEngine::Engine::Renderer
