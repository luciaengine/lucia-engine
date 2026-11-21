#include "Engine/Rendering/Impostors/MeshImpostorSystem.h"

#include "Assets/AssetManager.h"
#include "Assets/ModelAsset.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Engine/Rendering/Impostors/ImpostorBaker.h"
#include "Engine/Rendering/Impostors/ImpostorStage.h"
#include "Engine/Rendering/ModelRenderSetup.h"
#include "Engine/Rendering/RenderServices.h"
#include "Rendering/Core/Device.h"
#include "Types/Fnv1a.h"

#include <algorithm>
#include <string>

namespace GameEngine::Engine::Renderer
{
namespace
{

// Trees and foliage are seen from above and from the side: the hemisphere layout gives them
// twice the frames of the full sphere for the same atlas.
constexpr Mathematics::OctahedralLayout kLayout = Mathematics::OctahedralLayout::Hemisphere;

std::vector<Rendering::MeshGPUHandle> PartMeshes(const std::vector<MeshImpostorSystem::Part>& parts)
{
    std::vector<Rendering::MeshGPUHandle> meshes;
    meshes.reserve(parts.size());
    for (const MeshImpostorSystem::Part& part : parts)
        meshes.push_back(part.Mesh);
    return meshes;
}

uint64 MaterialHash(const std::vector<MeshImpostorSystem::Part>& parts)
{
    uint64 hash = Hashing::kFnv1a64OffsetBasis;
    for (const MeshImpostorSystem::Part& part : parts)
        hash = Hashing::Fnv1a64(part.Material.GetData().data(), part.Material.GetData().size(), hash);
    return hash;
}

} // namespace

MeshImpostorSystem::MeshImpostorSystem(RenderServices* renderServices)
    : m_RenderServices(renderServices)
{
}

MeshImpostorSystem::~MeshImpostorSystem()
{
    if (!m_RenderServices)
        return;
    for (auto& [model, impostor] : m_Models)
        UnlinkModel(impostor);
}

void MeshImpostorSystem::Update(ECS::World& world, float32 /*deltaTime*/)
{
    if (!m_RenderServices || !m_RenderServices->GetGPUScene())
        return;
    // The system runs on a job worker; registering the card quad and reading part entries take
    // the registry's table lock for the whole pass.
    Rendering::MeshGPURegistry::TableScope registryScope(m_RenderServices->GetMeshGPURegistry());

    // A device rebuild freed the meshes and the atlases: unlink, then scan and link again.
    const uint64 deviceGeneration =
        m_RenderServices->GetDevice() ? m_RenderServices->GetDevice()->GetDeviceRebuildGeneration() : 0u;
    const bool deviceRebuilt = deviceGeneration != m_DeviceGeneration;
    m_DeviceGeneration = deviceGeneration;
    if (deviceRebuilt)
    {
        for (auto& [model, impostor] : m_Models)
            UnlinkModel(impostor);
        m_Models.Clear();
    }

    const uint64 probeVersion = world.GetGlobalSystemVersion();
    const std::size_t structuralVersion = world.GetStructuralChangeVersion();
    bool changed = deviceRebuilt || structuralVersion != m_StructuralVersion;
    m_StructuralVersion = structuralVersion;
    {
        auto query = world.Query<ECS::Read<Components::MeshRenderer>>();
        query.Changed<Components::MeshRenderer>(m_MeshRendererGate);
        query.BatchEach([&changed](const Components::MeshRenderer*, std::size_t) { changed = true; });
        m_MeshRendererGate.LastRunVersion = probeVersion;
    }
    if (changed)
        Scan(world);
    if (m_Models.Empty())
        return;

    ++m_Frame;
    auto& stage = m_RenderServices->EnsureFeature<ImpostorStage>();
    stage.EnsureCardMesh(*m_RenderServices);
    auto& baker = m_RenderServices->EnsureFeature<ImpostorBaker>();
    std::vector<const Material*> cards;
    for (auto& [model, impostor] : m_Models)
    {
        if (const Material* card = UpdateModel(model, impostor, baker, stage))
            cards.push_back(card);
    }
    stage.SubmitCards(*m_RenderServices, world.GetWorldId(), cards);
}

void MeshImpostorSystem::Scan(ECS::World& world)
{
    // Opt-in state of each model met in this scan: absent from `models` = not yet looked up.
    FlatMap<GUID, bool> optedIn;
    FlatMap<GUID, ModelImpostor> models;
    AssetManager* assets = EngineCore::GetInstance().TryGetAssetManager();
    if (!assets)
        return;
    world.Query<ECS::Read<Components::MeshRenderer>>().Each(
        [&](ECS::EntityHandle, const Components::MeshRenderer& renderer)
        {
            if (renderer.meshGpuHandleId == 0u || renderer.modelAssetGuid.IsNull())
                return;
            const GUID modelGuid = renderer.modelAssetGuid.ToGuid();
            const bool* known = optedIn.Find(modelGuid);
            if (!known)
            {
                const SharedPtr<Asset> asset = assets->GetAsset(modelGuid);
                const auto* model = dynamic_cast<const ModelAsset*>(asset.get());
                const bool enabled = model && model->IsImpostorEnabled();
                known = &optedIn.InsertOrAssign(modelGuid, enabled);
                if (enabled)
                {
                    const Mathematics::BoundingBox box = ComputeModelBounds(*model).Box;
                    ModelImpostor& impostor = models.InsertOrAssign(modelGuid, ModelImpostor{});
                    impostor.Center = box.center;
                    impostor.Radius = box.Radius();
                    impostor.MeshHash = Hashing::HashCombine(model->GetSourceContentHash(),
                                                             std::hash<GUID>{}(modelGuid));
                }
            }
            if (!*known)
                return;
            ModelImpostor& impostor = *models.Find(modelGuid);
            const auto existing = std::find_if(impostor.Parts.begin(), impostor.Parts.end(),
                                               [&renderer](const Part& part) { return part.Submesh == renderer.meshId; });
            // The first entity that draws a part gives the material the atlas bakes with.
            if (existing == impostor.Parts.end())
                impostor.Parts.push_back(Part{renderer.meshId, Rendering::MeshGPUHandle(renderer.meshGpuHandleId),
                                              renderer.materialAssetGuid.ToGuid()});
        });
    for (auto& [model, impostor] : models)
        std::sort(impostor.Parts.begin(), impostor.Parts.end(),
                  [](const Part& a, const Part& b) { return a.Submesh < b.Submesh; });

    // Keep the link of a model whose parts did not change; unlink every other model.
    for (auto& [model, previous] : m_Models)
    {
        ModelImpostor* next = models.Find(model);
        const bool sameParts = next && next->Parts.size() == previous.Parts.size()
                               && std::equal(next->Parts.begin(), next->Parts.end(), previous.Parts.begin(),
                                             [](const Part& a, const Part& b)
                                             { return a.Mesh == b.Mesh && a.Material == b.Material; });
        if (sameParts)
        {
            next->LinkedMaterial = previous.LinkedMaterial;
            next->LinkedMeshes = std::move(previous.LinkedMeshes);
        }
        else
        {
            UnlinkModel(previous);
        }
    }
    m_Models = std::move(models);
}

const Material* MeshImpostorSystem::UpdateModel(const GUID& model, ModelImpostor& impostor, ImpostorBaker& baker,
                                                ImpostorStage& stage)
{
    ImpostorBakeRequest request{};
    request.Key.MeshHash = impostor.MeshHash;
    request.Key.MaterialHash = MaterialHash(impostor.Parts);
    request.Key.Layout = kLayout;
    request.Center = impostor.Center;
    request.Radius = impostor.Radius;
    for (const Part& part : impostor.Parts)
        request.Parts.push_back({part.Mesh, part.Material});
    // A request each frame keeps the atlas in the baker's least-recently-used budget.
    const ImpostorAtlas atlas = baker.Request(request, m_Frame);
    const GUID cardGuid = GUID::Derive(model, "impostor-card-" + std::to_string(request.Key.MaterialHash));
    const Material* card = ImpostorStage::RegisterCardMaterial(*m_RenderServices, cardGuid, "Impostor", atlas,
                                                               impostor.Center, impostor.Radius,
                                                               request.Key.FramesPerSide, kLayout);
    // Every part must be resident: a part that is not would draw its mesh beside the card.
    const std::vector<uint32> meshes = card ? ImpostorStage::PartMeshIndices(*m_RenderServices, PartMeshes(impostor.Parts))
                                            : std::vector<uint32>{};
    if (meshes.empty())
    {
        // Not baked, evicted, the card pipeline is still building, or a part is not resident: the
        // parts draw their meshes.
        UnlinkModel(impostor);
        return nullptr;
    }
    if (card->GetGpuSceneMaterialIndex() == impostor.LinkedMaterial && meshes == impostor.LinkedMeshes)
        return card;
    UnlinkModel(impostor);
    if (!stage.Link(*m_RenderServices, meshes, *card))
        return nullptr;
    impostor.LinkedMaterial = card->GetGpuSceneMaterialIndex();
    impostor.LinkedMeshes = meshes;
    return card;
}

void MeshImpostorSystem::UnlinkModel(ModelImpostor& impostor)
{
    if (impostor.LinkedMeshes.empty())
        return;
    ImpostorStage::Unlink(*m_RenderServices, impostor.LinkedMeshes);
    impostor.LinkedMaterial = 0xFFFFFFFFu;
    impostor.LinkedMeshes.clear();
}

} // namespace GameEngine::Engine::Renderer
