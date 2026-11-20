#pragma once

#include "AssetCore/GUID.h"
#include "ECS/ChangeFilter.h"
#include "ECS/Systems.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "Mathematics/Vector3.h"
#include "Types/FlatMap.h"
#include "Types/Types.h"

#include <cstddef>
#include <vector>

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

    /// One part of a model: a submesh with the material it bakes with.
    struct Part
    {
        uint32 Submesh = 0;
        Rendering::MeshGPUHandle Mesh;
        GUID Material;
    };

private:
    struct ModelImpostor
    {
        // Sorted by submesh index; Parts[0] is the primary part, which draws the card.
        std::vector<Part> Parts;
        Mathematics::Vector3 Center;
        float Radius = 0.0f;
        uint64 MeshHash = 0;
        // The card material index and the part mesh indices the link holds; empty while not linked.
        uint32 LinkedMaterial = 0xFFFFFFFFu;
        std::vector<uint32> LinkedMeshes;
    };

    // Rebuilds the parts of the opted-in models from the world's mesh renderers.
    void Scan(ECS::World& world);
    // Returns the card material once the parts are linked to it, else null.
    const Material* UpdateModel(const GUID& model, ModelImpostor& impostor, ImpostorBaker& baker,
                                ImpostorStage& stage);
    void UnlinkModel(ModelImpostor& impostor);

    RenderServices* m_RenderServices = nullptr;
    FlatMap<GUID, ModelImpostor> m_Models;
    ECS::ChangeGate m_MeshRendererGate;
    // A destroy that empties its chunk stamps no column, so the structural version also starts a scan.
    std::size_t m_StructuralVersion = static_cast<std::size_t>(-1);
    uint64 m_DeviceGeneration = 0;
    uint64 m_Frame = 0;
};

} // namespace GameEngine::Engine::Renderer
