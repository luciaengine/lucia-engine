#include "Engine/Rendering/Impostors/MeshImpostorSystem.h"

#include "Assets/AssetManager.h"
#include "Assets/ModelAsset.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/Impostors/ImpostorBaker.h"
#include "Engine/Rendering/Impostors/ImpostorStage.h"
#include "Engine/Rendering/ModelRenderSetup.h"
#include "Engine/Rendering/RenderServices.h"
#include "Logger/Logger.h"
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

bool CanBakeMaterial(const Material& material)
{
    using Rendering::MaterialKeyword;
    const MaterialKeyword keywords = material.GetVariantKey().materialKeywords;
    const MaterialAlphaMode alphaMode = material.GetAlphaMode();
    if ((alphaMode != MaterialAlphaMode::Opaque && alphaMode != MaterialAlphaMode::Mask)
        || Rendering::HasKeyword(keywords, MaterialKeyword::Transmission)
        || Rendering::HasKeyword(keywords, MaterialKeyword::DiffuseTransmission)
        || Rendering::HasKeyword(keywords, MaterialKeyword::Subsurface)
        || material.GetCompileSpec().customVertexShader
        || !material.GetDepthWriteOverride().value_or(true)
        || !material.GetDepthTestOverride().value_or(true)
        || !material.GetGraphicsPipelineId().IsValid())
        return false;
    for (uint32 slot = 0; slot < kTextureSlotArraySize; ++slot)
        if (material.IsTextureAwaited(slot))
            return false;
    return true;
}

} // namespace

MeshImpostorSystem::MeshImpostorSystem(RenderServices* renderServices)
    : m_RenderServices(renderServices)
{
    if (auto* assets = EngineCore::GetInstance().TryGetAssetManager())
        m_ModelReloadInvalidator = AssetReloadInvalidator(
            assets->GetEventDispatcher(), AssetType::Model,
            [this](const GUID&) { m_ModelReloaded.store(true, std::memory_order_relaxed); },
            AssetReloadInvalidator::EventSet::ReloadedOnly);
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
    const bool modelReloaded = m_ModelReloaded.exchange(false, std::memory_order_relaxed);
    bool changed = deviceRebuilt || modelReloaded || structuralVersion != m_StructuralVersion;
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

void MeshImpostorSystem::AddRenderer(ScanState& scan, const Components::MeshRenderer& renderer)
{
    if (renderer.meshGpuHandleId == 0u || renderer.modelAssetGuid.IsNull())
        return;
    const GUID modelGuid = renderer.modelAssetGuid.ToGuid();
    const SharedPtr<Asset>* source = scan.Sources.Find(modelGuid);
    if (!source)
    {
        SharedPtr<Asset> asset = scan.Assets->GetAsset(modelGuid);
        const auto* model = dynamic_cast<const ModelAsset*>(asset.get());
        if (model && model->IsImpostorEnabled() && model->GetMeshCount() > 0
            && !model->GetClusterLODManifest()
            && std::ranges::none_of(model->GetMeshes(), [](const Mesh& mesh)
               { return mesh.IsSkinned() || mesh.HasMorphTargets(); }))
        {
            const Mathematics::BoundingBox box = ComputeModelBounds(*model).Box;
            ModelImpostor& impostor = scan.Models.InsertOrAssign(modelGuid, ModelImpostor{});
            impostor.Parts.resize(model->GetMeshCount());
            impostor.Center = box.center;
            impostor.Radius = box.Radius();
            impostor.SwitchCoverage = model->GetImpostorSwitchCoverage();
            impostor.MeshHash = Hashing::HashCombine(model->GetSourceContentHash(),
                                                     std::hash<GUID>{}(modelGuid));
        }
        else
        {
            asset.reset();
        }
        source = &scan.Sources.InsertOrAssign(modelGuid, std::move(asset));
    }
    if (!*source)
        return;
    const auto& model = static_cast<const ModelAsset&>(**source);
    ModelImpostor& impostor = *scan.Models.Find(modelGuid);
    const uint32 submesh = ResolveSubmeshIndex(modelGuid, model, renderer);
    const Part part{Rendering::MeshGPUHandle(renderer.meshGpuHandleId), renderer.materialAssetGuid.ToGuid()};
    Part& existing = impostor.Parts[submesh];
    // The mesh row is shared by its instances, so all of them must agree on the bake.
    if (!existing.Mesh.IsValid())
        existing = part;
    else if (existing != part)
        impostor.HasConflictingParts = true;
}

void MeshImpostorSystem::Scan(ECS::World& world)
{
    // A null entry records a model checked but ineligible for a static bake.
    ScanState scan;
    scan.Assets = EngineCore::GetInstance().TryGetAssetManager();
    if (!scan.Assets)
        return;
    world.Query<ECS::Read<Components::MeshRenderer>>().Each(
        [&scan](ECS::EntityHandle, const Components::MeshRenderer& renderer) { AddRenderer(scan, renderer); });

    // Keep the link of a model whose parts did not change; unlink every other model.
    for (auto& [model, previous] : m_Models)
    {
        ModelImpostor* next = scan.Models.Find(model);
        const bool sameParts = next && !next->HasConflictingParts && next->SwitchCoverage == previous.SwitchCoverage
                               && next->MeshHash == previous.MeshHash && next->Parts == previous.Parts;
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
    m_Models = std::move(scan.Models);
}

const Material* MeshImpostorSystem::UpdateModel(const GUID& model, ModelImpostor& impostor, ImpostorBaker& baker,
                                                ImpostorStage& stage)
{
    if (impostor.HasConflictingParts)
    {
        UnlinkModel(impostor);
        return nullptr;
    }

    ImpostorBakeRequest request{};
    request.Key.MeshHash = impostor.MeshHash;
    const uint64 materialIdentity = MaterialHash(impostor.Parts);
    request.Key.MaterialHash = materialIdentity;
    request.Key.Layout = kLayout;
    request.Center = impostor.Center;
    request.Radius = impostor.Radius;
    const auto& materials = m_RenderServices->Materials().Registry();
    for (const Part& part : impostor.Parts)
    {
        const Material* material = part.Mesh.IsValid() && !part.Material.IsNull() ? materials.Find(part.Material) : nullptr;
        if (!material || !CanBakeMaterial(*material))
        {
            UnlinkModel(impostor);
            return nullptr;
        }
        request.Key.MaterialHash = Hashing::HashCombine(request.Key.MaterialHash, material->GetContentRevision());
        request.Key.MaterialHash = Hashing::HashCombine(request.Key.MaterialHash, material->GetVersion());
        request.Parts.push_back({part.Mesh, part.Material});
    }
    // A request each frame keeps the atlas in the baker's least-recently-used budget.
    const ImpostorAtlas atlas = baker.Request(request, m_Frame);
    const GUID cardGuid = GUID::Derive(model, "impostor-card-" + std::to_string(materialIdentity));
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
    if (!stage.Link(*m_RenderServices, meshes, *card, impostor.SwitchCoverage))
    {
        Logger::Log::Warning("MeshImpostorSystem: model {} could not link its {} parts to its card", model.ToString(),
                             meshes.size());
        return nullptr;
    }
    Logger::Log::Info("MeshImpostorSystem: model {} links {} parts to card material {}", model.ToString(),
                      meshes.size(), card->GetGpuSceneMaterialIndex());
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
