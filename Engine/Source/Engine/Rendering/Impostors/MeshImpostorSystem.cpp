#include "Engine/Rendering/Impostors/MeshImpostorSystem.h"

#include "Assets/AssetManager.h"
#include "Assets/ModelAsset.h"
#include "Components/Rendering/MeshGPUData.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Transform.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "../Forest/ForestPlacementFeature.h"
#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/MaterialRegistry.h"
#include "Engine/Rendering/Impostors/ImpostorBaker.h"
#include "Engine/Rendering/Impostors/ImpostorStage.h"
#include "Engine/Rendering/ModelRenderSetup.h"
#include "Engine/Rendering/RenderServices.h"
#include "Logger/Logger.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/GPUScene.h"
#include "Types/Fnv1a.h"

#include <algorithm>
#include <cmath>
#include <optional>
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

// The reason a material can never bake, or empty when it can.
std::string_view MaterialRefusal(const Material& material)
{
    using Rendering::MaterialKeyword;
    const MaterialKeyword keywords = material.GetVariantKey().materialKeywords;
    const MaterialAlphaMode alphaMode = material.GetAlphaMode();
    if (alphaMode != MaterialAlphaMode::Opaque && alphaMode != MaterialAlphaMode::Mask)
        return "a part material is not opaque or masked";
    if (Rendering::HasKeyword(keywords, MaterialKeyword::Transmission)
        || Rendering::HasKeyword(keywords, MaterialKeyword::DiffuseTransmission)
        || Rendering::HasKeyword(keywords, MaterialKeyword::Subsurface))
        return "a part material uses transmission or subsurface scattering";
    if (material.GetCompileSpec().customVertexShader)
        return "a part material has a custom vertex shader";
    if (!material.GetDepthWriteOverride().value_or(true) || !material.GetDepthTestOverride().value_or(true))
        return "a part material turns off the depth write or the depth test";
    return {};
}

// The reason an opted-in model can never take a card, or empty when it can.
std::string_view ModelRefusal(const ModelAsset& model)
{
    if (model.GetClusterLODManifest())
        return "the model uses cluster LOD";
    if (std::ranges::any_of(model.GetMeshes(), [](const Mesh& mesh) { return mesh.IsSkinned(); }))
        return "the model is skinned";
    if (std::ranges::any_of(model.GetMeshes(), [](const Mesh& mesh) { return mesh.HasMorphTargets(); }))
        return "the model has morph targets";
    return {};
}

// The foliage_wind.glsl sway that every part shares. Parts without the modifier do not sway.
std::optional<ImpostorFoliageSway> SharedFoliageSway(const std::vector<MeshImpostorSystem::Part>& parts,
                                                     const MaterialRegistry& materials)
{
    std::optional<ImpostorFoliageSway> shared;
    for (const auto& part : parts)
    {
        const Material* material = materials.Find(part.Material);
        if (!material)
            return std::nullopt;
        ImpostorFoliageSway sway{};
        if (material->GetCompileSpec().vertexModifierPath == "VertexModifiers/foliage_wind.glsl")
        {
            Mathematics::Vector2 motion{};
            material->GetVector(HashStringId("foliageWindMotion"), motion.v, 2u);
            Mathematics::Vector2 range{sway.BaseY, sway.Height};
            material->GetVector(HashStringId("foliageWindHeight"), range.v, 2u);
            sway = {range.x, range.y, motion.x};
        }
        const bool valid = std::isfinite(sway.BaseY) && std::isfinite(sway.Height) && sway.Height > 0.0f &&
                           std::isfinite(sway.MaxDisplacement) && sway.MaxDisplacement >= 0.0f;
        if (!valid || (shared && *shared != sway))
            return std::nullopt;
        shared = sway;
    }
    return shared;
}

// The placement key of a renderer: a hash of its exact world transform. The parts of one placement
// share the transform bit for bit.
uint64 PlacementKey(const Components::WorldTransform& transform)
{
    uint64 hash = Hashing::Fnv1a64Value(Hashing::kFnv1a64OffsetBasis, transform.Position);
    return Hashing::Fnv1a64Value(hash, transform.Basis);
}

// True when the material's pipeline is built and its textures are loaded.
bool IsMaterialReady(const Material& material)
{
    if (!material.GetGraphicsPipelineId().IsValid())
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
    m_Models.Clear();
    ApplyOwners();
}

void MeshImpostorSystem::Update(ECS::World& world, float32 /*deltaTime*/)
{
    if (!m_RenderServices || !m_RenderServices->GetGPUScene())
        return;
    if (auto* forest = m_RenderServices->GetFeature<ForestPlacementFeature>())
        forest->ClearCards(world.GetWorldId());
    // Registering the card quad and reading part entries take the registry's table lock for the whole pass.
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
    const bool rescanDue = m_RescanPending && m_UpdatesUntilRescan == 0;
    if (m_UpdatesUntilRescan > 0)
        --m_UpdatesUntilRescan;
    bool changed = deviceRebuilt || modelReloaded || rescanDue || structuralVersion != m_StructuralVersion;
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

    auto& stage = m_RenderServices->EnsureFeature<ImpostorStage>();
    stage.EnsureCardMesh(*m_RenderServices);
    auto& baker = m_RenderServices->EnsureFeature<ImpostorBaker>();
    std::vector<const Material*> cards;
    for (auto& [model, impostor] : m_Models)
    {
        if (const Material* card = UpdateModel(model, impostor, baker, stage, world.GetWorldId()))
            cards.push_back(card);
    }
    stage.SubmitCards(*m_RenderServices, world.GetWorldId(), cards);
}

void MeshImpostorSystem::AddRenderer(ScanState& scan, const Components::MeshRenderer& renderer,
                                     const Components::WorldTransform& transform, uint32 instanceIndex)
{
    if (renderer.meshGpuHandleId == 0u || renderer.modelAssetGuid.IsNull())
        return;
    const GUID modelGuid = renderer.modelAssetGuid.ToGuid();
    const SharedPtr<Asset>* source = scan.Sources.Find(modelGuid);
    if (!source)
    {
        SharedPtr<Asset> asset = scan.Assets->GetAsset(modelGuid);
        const auto* model = dynamic_cast<const ModelAsset*>(asset.get());
        if (model && model->IsImpostorEnabled() && model->GetMeshCount() > 0 && ModelRefusal(*model).empty())
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
            if (model && model->IsImpostorEnabled() && model->GetMeshCount() > 0)
                scan.Refusals.InsertOrAssign(modelGuid, ModelRefusal(*model));
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

    if (instanceIndex == kNoInstance)
    {
        scan.MissingInstance = true;
        return;
    }
    Placement& placement = impostor.Placements.GetOrInsert(PlacementKey(transform));
    placement.Instances.resize(impostor.Parts.size(), kNoInstance);
    if (placement.Instances[submesh] != kNoInstance)
        placement.HasDuplicatePart = true;
    else
        placement.Instances[submesh] = instanceIndex;
}

void MeshImpostorSystem::ApplyOwners()
{
    Rendering::GPUScene* scene = m_RenderServices ? m_RenderServices->GetGPUScene() : nullptr;
    if (!scene)
        return;
    FlatMap<uint32, uint32> owners;
    for (const auto& [model, impostor] : m_Models)
    {
        for (const auto& [key, placement] : impostor.Placements)
        {
            // Part 0 is the primary part of the link, so its instance draws the card.
            const bool complete = !placement.HasDuplicatePart
                                  && std::ranges::none_of(placement.Instances,
                                                          [](uint32 instance) { return instance == kNoInstance; });
            if (!complete)
                continue;
            for (const uint32 instance : placement.Instances)
                owners.InsertOrAssign(instance, placement.Instances[0]);
        }
    }
    for (const auto& [instance, owner] : m_Owners)
        if (!owners.Find(instance))
            scene->SetInstanceImpostorOwner(instance, Rendering::GPUScene::kNoImpostorOwner);
    for (const auto& [instance, owner] : owners)
        scene->SetInstanceImpostorOwner(instance, owner);
    m_Owners = std::move(owners);
}

void MeshImpostorSystem::Scan(ECS::World& world)
{
    // A null entry records a model checked but ineligible for a static bake.
    ScanState scan;
    scan.Assets = EngineCore::GetInstance().TryGetAssetManager();
    if (!scan.Assets)
        return;
    world.Query<ECS::Read<Components::MeshRenderer>, ECS::Read<Components::WorldTransform>,
                ECS::Optional<Components::MeshGPUData>>()
        .Each([&scan](ECS::EntityHandle, const Components::MeshRenderer& renderer,
                      const Components::WorldTransform& transform, const Components::MeshGPUData* gpu)
              { AddRenderer(scan, renderer, transform, gpu ? gpu->instanceIndex : kNoInstance); });

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
    m_RescanPending = scan.MissingInstance;
    m_UpdatesUntilRescan = scan.MissingInstance ? kRescanInterval : 0;
    ApplyOwners();
    for (const auto& [model, reason] : scan.Refusals)
        LogRefusal(model, reason);
}

void MeshImpostorSystem::LogRefusal(const GUID& model, std::string_view reason)
{
    if (m_RefusalsLogged.Insert(model))
        Logger::Log::Warning("MeshImpostorSystem: model {} keeps its meshes at every distance: {}", model.ToString(),
                             reason);
}

const Material* MeshImpostorSystem::UpdateModel(const GUID& model, ModelImpostor& impostor, ImpostorBaker& baker,
                                                ImpostorStage& stage, uint64 worldId)
{
    if (impostor.HasConflictingParts)
    {
        UnlinkModel(impostor);
        LogRefusal(model, "instances of one part use different meshes or materials");
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
    bool hasTwoSidedCutout = false;
    for (const Part& part : impostor.Parts)
    {
        const Material* material = part.Mesh.IsValid() && !part.Material.IsNull() ? materials.Find(part.Material) : nullptr;
        const std::string_view refusal = material ? MaterialRefusal(*material) : std::string_view{};
        if (!refusal.empty())
            LogRefusal(model, refusal);
        if (!material || !refusal.empty() || !IsMaterialReady(*material))
        {
            UnlinkModel(impostor);
            return nullptr;
        }
        request.Key.MaterialHash = Hashing::HashCombine(request.Key.MaterialHash, material->GetContentRevision());
        request.Key.MaterialHash = Hashing::HashCombine(request.Key.MaterialHash, material->GetVersion());
        request.Parts.push_back({part.Mesh, part.Material});
        hasTwoSidedCutout |= material->IsDoubleSided() && material->GetAlphaMode() == MaterialAlphaMode::Mask;
    }
    // A request each frame keeps the atlas in the baker's least-recently-used budget.
    const ImpostorAtlas atlas = baker.Request(request, *m_RenderServices);
    // Imported two-sided cutouts light both leaf faces. Averaging those normals
    // in the atlas loses some of that direct light before the crown shadow is applied.
    const float normalCoherenceWeight = hasTwoSidedCutout ? 0.65f : 1.0f;
    PublishForestCard(model, impostor, atlas, request, worldId, normalCoherenceWeight);
    const GUID cardGuid = GUID::Derive(model, "impostor-card-" + std::to_string(materialIdentity));
    const Material* card = ImpostorStage::RegisterCardMaterial(*m_RenderServices, cardGuid, "Impostor", atlas,
                                                               impostor.Center, impostor.Radius,
                                                               request.Key.FramesPerSide, kLayout,
                                                               ImpostorCardSource::SceneInstance, normalCoherenceWeight);
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
    m_RefusalsLogged.Erase(model);
    impostor.LinkedMaterial = card->GetGpuSceneMaterialIndex();
    impostor.LinkedMeshes = meshes;
    return card;
}

void MeshImpostorSystem::PublishForestCard(const GUID& model, const ModelImpostor& impostor,
                                           const ImpostorAtlas& atlas, const ImpostorBakeRequest& request,
                                           uint64 worldId, float normalCoherenceWeight)
{
    auto* forest = m_RenderServices->GetFeature<ForestPlacementFeature>();
    if (!forest || !forest->WantsCard(worldId, model))
        return;
    const auto sway = SharedFoliageSway(impostor.Parts, m_RenderServices->Materials().Registry());
    if (!sway)
    {
        LogRefusal(model, "its parts use different foliage wind settings, so a packed forest draws it as a stand");
        return;
    }
    const GUID cardGuid = GUID::Derive(model, "forest-impostor-" + std::to_string(request.Key.MaterialHash));
    if (!ImpostorStage::RegisterCardMaterial(*m_RenderServices, cardGuid, "Forest Impostor", atlas, impostor.Center,
                                             impostor.Radius, request.Key.FramesPerSide, kLayout,
                                             ImpostorCardSource::PackedForest, normalCoherenceWeight))
        return;
    const ForestImpostorCard card{cardGuid,
                                  {impostor.Center.x, impostor.Center.y, impostor.Center.z, impostor.Radius},
                                  *sway, impostor.SwitchCoverage};
    forest->PublishCard(worldId, model, card, request.Parts);
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
