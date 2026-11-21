#include "Engine/Rendering/Impostors/MeshImpostorSystem.h"

#include "Platform/Environment.h"

#include "Assets/AssetManager.h"
#include "Assets/ModelAsset.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Rendering/LODGroup.h"
#include "Components/Rendering/LocalBounds.h"
#include "Components/Rendering/MeshGPUData.h"
#include "Components/SceneViewHidden.h"
#include "Components/Transform.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/Impostors/ImpostorBaker.h"
#include "Engine/Rendering/Impostors/ImpostorStage.h"
#include "Engine/Rendering/Forest/ForestPlacementFeature.h"
#include "Engine/Rendering/MeshLODThresholds.h"
#include "Engine/Rendering/RenderOrigin.h"
#include "Rendering/Common/Frustum.h"
#include "Rendering/Common/MatrixUtils.h"
#include "Engine/Rendering/ModelRenderSetup.h"
#include "Engine/Rendering/RenderServices.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/GPUScene.h"
#include "Types/Fnv1a.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <tuple>
#include <string>

namespace GameEngine::Engine::Renderer
{
namespace
{

// Trees and foliage are seen from above and from the side: the hemisphere layout gives them
// twice the frames of the full sphere for the same atlas.
constexpr Mathematics::OctahedralLayout kLayout = Mathematics::OctahedralLayout::Hemisphere;
// Warm the atlas before a shrinking placement first reaches the switch.
constexpr float kAtlasCoverageLead = 1.5f;
// Extreme authored bias values stay in the exact path, where exp2 uses the summed bias.
constexpr float kAtlasBroadphaseBiasLimit = 32.0f;
constexpr size_t kAtlasDemandLeafSize = 128;

struct AtlasDemandView
{
    Mathematics::WorldPosition Origin;
    Mathematics::Vector3 Eye;
    Rendering::Vector4 Planes[6];
    float ProjectionScale;
    float DefaultSwitch;
    float GlobalLodBias;
    uint32 Layers;
    bool DropEditorHidden;
};

bool NeedsAtlas(const AtlasDemandView& view, const Mathematics::WorldPosition& position,
                float radius, float lodBias, uint32 layers, bool hidden, float authoredSwitch)
{
    if ((view.Layers & layers) == 0u || (view.DropEditorHidden && hidden))
        return false;
    const auto center = position - view.Origin;
    const float distance = std::max((center - view.Eye).Length(), 1.0e-4f);
    const float coverage = radius * view.ProjectionScale / distance * std::exp2(lodBias + view.GlobalLodBias);
    const float threshold = authoredSwitch > 0.0f ? authoredSwitch : view.DefaultSwitch;
    return coverage < threshold * kAtlasCoverageLead &&
           Rendering::TestSphereFrustum(center, radius, view.Planes);
}

bool NeedsAtlasDetail(const AtlasDemandView& view, const Mathematics::WorldPosition& position,
                      float centerRadius, float maxRadius, float minBiasedRadius, float authoredSwitch)
{
    const auto center = position - view.Origin;
    const float farthest = std::max((center - view.Eye).Length() + centerRadius, 1.0e-4f);
    const float minCoverage = minBiasedRadius * view.ProjectionScale / farthest * std::exp2(view.GlobalLodBias);
    const float threshold = authoredSwitch > 0.0f ? authoredSwitch : view.DefaultSwitch;
    // Keep marginal cases in the exact pass, including float rounding around the switch.
    const bool allTooLarge = std::abs(view.GlobalLodBias) <= kAtlasBroadphaseBiasLimit &&
                             std::isfinite(minCoverage) && minCoverage > threshold * kAtlasCoverageLead * 1.0001f;
    return !allTooLarge &&
           Rendering::TestSphereFrustum(center, centerRadius + maxRadius, view.Planes);
}

struct PlacementPart
{
    GUID Model;
    std::array<unsigned char, Components::kTransformPoseBytes> Pose;
    uint32 Layers;
    uint32 LodBias;
    bool CastShadows;
    bool EditorHidden;
    ECS::EntityHandle Entity;
    uint32 Submesh;
    uint32 InstanceIndex;

    auto Key() const { return std::tie(Model, Pose, Layers, LodBias, CastShadows, EditorHidden); }
};

template <typename Component>
bool PlacementInputChanged(ECS::World& world, ECS::ChangeGate& gate, uint64 version)
{
    bool changed = false;
    auto query = world.Query<ECS::Read<Component>, ECS::Read<Components::MeshRenderer>>();
    query.template Changed<Component>(gate);
    query.BatchEach([&changed](const Component*, const Components::MeshRenderer*, std::size_t) { changed = true; });
    gate.LastRunVersion = version;
    return changed;
}

bool IsPlacementMemberBefore(ECS::EntityHandle left, ECS::EntityHandle right)
{
    return left.id < right.id;
}

bool SetPlacementPrimary(ECS::World& world, Rendering::GPUScene& scene, ECS::EntityHandle entity, uint32 primaryPlusOne)
{
    if (!world.IsValid(entity))
        return false;
    const auto* old = world.GetComponent<Components::MeshGPUData>(entity);
    if (!old || old->impostorPlacementPrimaryPlusOne == primaryPlusOne)
        return false;
    auto* gpu = world.GetComponentForWrite<Components::MeshGPUData>(entity);
    if (!gpu)
        return false;
    gpu->impostorPlacementPrimaryPlusOne = primaryPlusOne;
    scene.RequestInstanceHistoryReset(gpu->instanceIndex);
    return true;
}

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
    auto* forest = m_RenderServices ? m_RenderServices->GetFeature<ForestPlacementFeature>() : nullptr;
    if (forest)
        forest->ClearCards(world.GetWorldId());
    if (!m_RenderServices || !m_RenderServices->GetGPUScene() || !ImpostorStage::IsEnabled(*m_RenderServices))
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
    if (!m_Models.Empty() || !m_PlacementMembers.empty())
    {
        const uint64 sceneEpoch = m_RenderServices->GetGPUScene()->GetContentEpoch();
        bool placementChanged = changed || (m_WaitingForPlacementSlots && sceneEpoch != m_PlacementSceneEpoch);
        m_PlacementSceneEpoch = sceneEpoch;
        placementChanged |= PlacementInputChanged<Components::WorldTransform>(world, m_PlacementTransformGate, probeVersion);
        placementChanged |= PlacementInputChanged<Components::LocalBounds>(world, m_PlacementBoundsGate, probeVersion);
        placementChanged |= PlacementInputChanged<Components::LODGroup>(world, m_PlacementLodGate, probeVersion);
        placementChanged |= m_RenderServices->HasPendingResidencyExtraction();
        if (placementChanged)
            UpdatePlacements(world);
    }
    if (m_Models.Empty())
        return;

    auto& stage = m_RenderServices->EnsureFeature<ImpostorStage>();
    stage.EnsureCardMesh(*m_RenderServices);
    auto& baker = m_RenderServices->EnsureFeature<ImpostorBaker>(m_RenderServices->GetProfile().ImpostorAtlasBudgetBytes);
    RefreshAtlasDemand(world.GetWorldId());
    std::vector<const Material*> cards;
    for (auto& [model, impostor] : m_Models)
    {
        const auto worldId = world.GetWorldId();
        const bool wantsForestAtlas = forest && forest->WantsAtlas(worldId, model);
        const bool wantsForestShadowSources = forest && forest->ConsumeShadowSourceDemand(worldId, model);
        if (!impostor.WantsAtlas && !wantsForestAtlas && !wantsForestShadowSources)
        {
            // Drop the link before this undemanded atlas can be evicted.
            UnlinkModel(impostor);
            continue;
        }
        if (const Material* card = UpdateModel(model, impostor, baker, stage, forest, worldId,
                                               wantsForestAtlas, wantsForestShadowSources))
            cards.push_back(card);
    }
    stage.SubmitCards(*m_RenderServices, world.GetWorldId(), cards);
}

void MeshImpostorSystem::RefreshAtlasDemand(uint64 world)
{
    const auto& services = *m_RenderServices;
    const auto& views = services.Views();
    // Mesh links are shared across worlds. Preserve existing atlas demand when
    // another visible world can be drawing through these same links.
    const bool sharedViewLinks = std::ranges::any_of(views.GetViews(), [world](const Rendering::ViewDesc& view)
    {
        return view.worldId != 0u && view.worldId != world && view.RenderedByPipeline();
    });
    for (auto& [guid, model] : m_Models)
        model.WantsAtlas = sharedViewLinks && !model.Placements.empty();
    if (sharedViewLinks || services.GetMeshGPURegistry().GetLodSelectionMode() == Rendering::LodSelectionMode::Off)
        return;
    for (const auto& view : views.GetViews())
    {
        if ((view.worldId != 0 && view.worldId != world) ||
            (!view.RenderedByPipeline() && !(view.purpose == Rendering::ViewPurpose::UtilityCapture && view.worldId != 0)) ||
            services.ResolveViewForceLOD(view.id) != 0xFFFFFFFFu)
            continue;
        const auto* camera = views.FindCameraData(view.cameraId);
        if (!camera || camera->cameraPos.w == 1.0f)
            continue;
        const float projection = std::abs(camera->proj[1][1]);
        if (!(projection > 0.0f))
            continue;
        AtlasDemandView item{};
        item.Origin = Mathematics::WorldPosition::FromSector(CameraRenderOriginSector(*camera), {});
        item.Eye = CameraEyeRelativeToOrigin(*camera);
        Rendering::ExtractFrustumPlanes(camera->viewProjRel, item.Planes);
        item.ProjectionScale = projection;
        item.DefaultSwitch = ImpostorStage::DefaultSwitchCoverage(services, view.id);
        item.GlobalLodBias = services.GetLODGlobalBias();
        item.Layers = view.ActiveRenderLayerMask().Bits;
        item.DropEditorHidden = view.DropsEditorHidden();
        for (auto& entry : m_Models)
        {
            auto& model = entry.second;
            for (size_t nodeIndex = 0; nodeIndex < model.DemandTree.size() && !model.WantsAtlas;)
            {
                const auto& node = model.DemandTree[nodeIndex];
                const auto& bounds = node.Bounds;
                const uint32 layers = item.DropEditorHidden ? bounds.EditorLayers : bounds.Layers;
                if ((item.Layers & layers) == 0u ||
                    !NeedsAtlasDetail(item, bounds.Center, bounds.CenterRadius, bounds.MaxRadius,
                                      bounds.MinBiasedRadius, model.SwitchCoverage))
                {
                    nodeIndex = node.SubtreeEnd;
                    continue;
                }
                if (node.Count != 0)
                {
                    model.WantsAtlas = std::ranges::any_of(
                        std::span<const PlacementDemand>(model.Placements).subspan(node.First, node.Count),
                        [&](const PlacementDemand& placement)
                        {
                            return NeedsAtlas(item, placement.Center, placement.Radius, placement.LodBias,
                                              placement.Layers, placement.EditorHidden, model.SwitchCoverage);
                        });
                }
                ++nodeIndex;
            }
        }
    }
}

void MeshImpostorSystem::Scan(ECS::World& world)
{
    const auto* assets = EngineCore::GetInstance().TryGetAssetManager();
    if (!assets)
        return;
    // A null entry records a model checked but ineligible for a static bake.
    FlatMap<GUID, SharedPtr<Asset>> sources;
    FlatMap<GUID, ModelImpostor> models;
    world.Query<ECS::Read<Components::MeshRenderer>>().Each(
        [&](ECS::EntityHandle, const Components::MeshRenderer& renderer)
        {
            if (renderer.meshGpuHandleId == 0u || renderer.modelAssetGuid.IsNull())
                return;
            const GUID modelGuid = renderer.modelAssetGuid.ToGuid();
            const SharedPtr<Asset>* source = sources.Find(modelGuid);
            if (!source)
            {
                SharedPtr<Asset> asset = assets->GetAsset(modelGuid);
                const auto* model = dynamic_cast<const ModelAsset*>(asset.get());
                if (model && model->IsImpostorEnabled() && model->GetMeshCount() > 0
                    && std::ranges::none_of(model->GetMeshes(), [](const Mesh& mesh)
                       { return mesh.IsSkinned() || mesh.HasMorphTargets(); }))
                {
                    const Mathematics::BoundingBox box = ComputeModelBounds(*model).Box;
                    ModelImpostor& impostor = models.InsertOrAssign(modelGuid, ModelImpostor{});
                    impostor.Parts.resize(model->GetMeshCount());
                    impostor.Center = box.center;
                    impostor.Radius = box.Radius();
                    impostor.SwitchCoverage = model->GetImpostorSwitchCoverage();
                    impostor.Clustered = model->GetClusterLODManifest() != nullptr;
                    impostor.MeshHash = Hashing::HashCombine(model->GetSourceContentHash(),
                                                             std::hash<GUID>{}(modelGuid));
                }
                else
                {
                    asset.reset();
                }
                source = &sources.InsertOrAssign(modelGuid, std::move(asset));
            }
            if (!*source)
                return;
            const auto& model = static_cast<const ModelAsset&>(**source);
            ModelImpostor& impostor = *models.Find(modelGuid);
            const uint32 submesh = ResolveSubmeshIndex(modelGuid, model, renderer);
            const Part part{Rendering::MeshGPUHandle(renderer.meshGpuHandleId), renderer.materialAssetGuid.ToGuid()};
            // Only the whole resident cut can supply a card. A streamed unit has a different key.
            if (impostor.Clustered &&
                part.Mesh != m_RenderServices->GetMeshGPURegistry().FindHandle({modelGuid, submesh}))
                return;
            Part& existing = impostor.Parts[submesh];
            // The mesh row is shared by its instances, so all of them must agree on the bake.
            if (!existing.Mesh.IsValid())
                existing = part;
            else if (existing != part)
                impostor.HasConflictingParts = true;
        });

    // Keep the link of a model whose parts did not change; unlink every other model.
    for (auto& [model, previous] : m_Models)
    {
        ModelImpostor* next = models.Find(model);
        const bool sameParts = next && !next->HasConflictingParts && next->Clustered == previous.Clustered
                               && next->SwitchCoverage == previous.SwitchCoverage
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
    m_Models = std::move(models);
}

MeshImpostorSystem::PlacementDemandBounds MeshImpostorSystem::BuildPlacementDemandBounds(
    std::span<const PlacementDemand> placements, double Mathematics::WorldPosition::*& splitAxis)
{
    PlacementDemandBounds bounds{};
    Mathematics::WorldPosition low = placements.front().Center;
    Mathematics::WorldPosition high = low;
    float minBiasedRadius = std::numeric_limits<float>::infinity();
    for (const auto& placement : placements)
    {
        const auto& center = placement.Center;
        low.X = std::min(low.X, center.X);
        low.Y = std::min(low.Y, center.Y);
        low.Z = std::min(low.Z, center.Z);
        high.X = std::max(high.X, center.X);
        high.Y = std::max(high.Y, center.Y);
        high.Z = std::max(high.Z, center.Z);
        bounds.MaxRadius = std::max(bounds.MaxRadius, placement.Radius);
        const float biasedRadius = std::abs(placement.LodBias) <= kAtlasBroadphaseBiasLimit
            ? placement.Radius * std::exp2(placement.LodBias) : 0.0f;
        minBiasedRadius = std::min(minBiasedRadius, biasedRadius);
        bounds.Layers |= placement.Layers;
        if (!placement.EditorHidden)
            bounds.EditorLayers |= placement.Layers;
    }
    const double x = high.X - low.X;
    const double y = high.Y - low.Y;
    const double z = high.Z - low.Z;
    splitAxis = &Mathematics::WorldPosition::X;
    double extent = x;
    if (y > extent)
    {
        extent = y;
        splitAxis = &Mathematics::WorldPosition::Y;
    }
    if (z > extent)
        splitAxis = &Mathematics::WorldPosition::Z;

    bounds.Center = {(low.X + high.X) * 0.5, (low.Y + high.Y) * 0.5, (low.Z + high.Z) * 0.5};
    bounds.CenterRadius = (high - low).Length() * 0.5f;
    bounds.MinBiasedRadius = std::isfinite(minBiasedRadius) ? minBiasedRadius : 0.0f;
    return bounds;
}

void MeshImpostorSystem::BuildPlacementDemandTree(ModelImpostor& model, size_t first, size_t count)
{
    auto placements = std::span<PlacementDemand>(model.Placements).subspan(first, count);
    const size_t nodeIndex = model.DemandTree.size();
    double Mathematics::WorldPosition::* axis = nullptr;
    const auto bounds = BuildPlacementDemandBounds(placements, axis);
    model.DemandTree.push_back({bounds, nodeIndex + 1, first, count});
    if (count <= kAtlasDemandLeafSize)
        return;

    // NaN centers sort last so invalid authored positions cannot break the partition order.
    const size_t middle = count / 2;
    std::ranges::nth_element(placements, placements.begin() + static_cast<ptrdiff_t>(middle),
        [axis](const PlacementDemand& a, const PlacementDemand& b)
        {
            const double left = a.Center.*axis;
            const double right = b.Center.*axis;
            return left < right || (!std::isnan(left) && std::isnan(right));
        });

    BuildPlacementDemandTree(model, first, middle);
    BuildPlacementDemandTree(model, first + middle, count - middle);
    auto& node = model.DemandTree[nodeIndex];
    node.SubtreeEnd = model.DemandTree.size();
    node.Count = 0;
}

void MeshImpostorSystem::UpdatePlacements(ECS::World& world)
{
    for (auto& [guid, model] : m_Models)
        model.Placements.clear();
    const auto placementDemand = [](const Components::WorldTransform& transform,
                                    const Components::LocalBounds* bounds,
                                    float lodBias, uint32 layers, bool hidden)
    {
        if (!bounds)
            return PlacementDemand{transform.GetPosition(), 1.0f, lodBias, layers, hidden};
        const auto matrix = transform.ToMatrix4x4({});
        return PlacementDemand{transform.GetPosition() + matrix.TransformPoint(bounds->Box.center),
                               bounds->Box.Radius() * Rendering::MatrixUtils::LargestAxisScale(matrix.Data()),
                               lodBias, layers, hidden};
    };
    std::vector<PlacementPart> parts;
    m_WaitingForPlacementSlots = false;
    if (!m_Models.Empty())
    {
        world.Query<ECS::Read<Components::MeshRenderer>, ECS::Read<Components::WorldTransform>,
                    ECS::Read<Components::MeshGPUData>, ECS::Optional<Components::LocalBounds>,
                    ECS::Optional<Components::LODGroup>, ECS::Optional<Components::Transform>,
                    ECS::Optional<Components::SceneViewHidden>,
                    ECS::Optional<Components::SceneViewHiddenInHierarchy>>().Each(
            [&](ECS::EntityHandle entity, const Components::MeshRenderer& renderer,
                const Components::WorldTransform& transform, const Components::MeshGPUData& gpu,
                const Components::LocalBounds* bounds, const Components::LODGroup* lod,
                const Components::Transform* local, const Components::SceneViewHidden* hidden,
                const Components::SceneViewHiddenInHierarchy* hiddenInHierarchy)
            {
                if (gpu.hlodEvicted || gpu.clusterEvicted || gpu.clusterSplitOwner || renderer.meshGpuHandleId == 0u
                    || renderer.shadowCasting == Components::ShadowCasting::ShadowsOnly
                    || (transform.Version == 0u && local))
                    return;
                const GUID guid = renderer.modelAssetGuid.ToGuid();
                auto* model = m_Models.Find(guid);
                if (!model || model->HasConflictingParts)
                    return;
                // Match the bound submesh, including named source-node selectors.
                const auto part = std::ranges::find(model->Parts, Rendering::MeshGPUHandle(renderer.meshGpuHandleId),
                                                    &Part::Mesh);
                if (part == model->Parts.end())
                    return;
                if (!model->RequiresPlacement())
                {
                    model->Placements.push_back(placementDemand(transform, bounds, lod ? lod->Bias : 0.0f,
                                                                      renderer.RenderLayers.Bits, hidden || hiddenInHierarchy));
                    return;
                }
                if (!bounds || bounds->Box.center.x != model->Center.x || bounds->Box.center.y != model->Center.y
                    || bounds->Box.center.z != model->Center.z || bounds->Box.Radius() != model->Radius)
                    return;
                PlacementPart placement{guid, {}, renderer.RenderLayers.Bits,
                                        std::bit_cast<uint32>(lod ? lod->Bias : 0.0f),
                                        bounds->CastShadows && renderer.shadowCasting != Components::ShadowCasting::Off,
                                        hidden || hiddenInHierarchy, entity, static_cast<uint32>(part - model->Parts.begin()),
                                        gpu.instanceIndex};
                std::memcpy(placement.Pose.data(), &transform, placement.Pose.size());
                parts.push_back(placement);
            });
    }

    std::ranges::sort(parts, [](const PlacementPart& a, const PlacementPart& b)
    {
        if (a.Key() != b.Key())
            return a.Key() < b.Key();
        return a.Submesh < b.Submesh;
    });
    auto& scene = *m_RenderServices->GetGPUScene();
    std::vector<ECS::EntityHandle> members;
    bool extractionNeeded = false;
    for (size_t first = 0; first < parts.size();)
    {
        size_t last = first + 1;
        while (last < parts.size() && parts[last].Key() == parts[first].Key())
            ++last;
        // A duplicate submesh makes a co-located placement ambiguous.
        auto& model = *m_Models.Find(parts[first].Model);
        bool complete = last - first == model.Parts.size();
        for (size_t i = first; complete && i < last; ++i)
            complete = parts[i].Submesh == i - first;
        if (complete)
        {
            const auto& part = parts[first];
            const auto* transform = world.GetComponent<Components::WorldTransform>(part.Entity);
            const auto* bounds = world.GetComponent<Components::LocalBounds>(part.Entity);
            model.Placements.push_back(placementDemand(*transform, bounds, std::bit_cast<float>(part.LodBias),
                                                       part.Layers, part.EditorHidden));
            const uint32 primary = part.InstanceIndex;
            m_WaitingForPlacementSlots |= primary >= scene.GetInstances().size();
            if (primary < scene.GetInstances().size())
            {
                for (size_t i = first; i < last; ++i)
                {
                    const auto entity = parts[i].Entity;
                    members.push_back(entity);
                    extractionNeeded |= SetPlacementPrimary(world, scene, entity, primary + 1u);
                }
            }
        }
        first = last;
    }
    std::ranges::sort(members, IsPlacementMemberBefore);
    for (const auto entity : m_PlacementMembers)
    {
        if (!std::ranges::binary_search(members, entity, IsPlacementMemberBefore))
            extractionNeeded |= SetPlacementPrimary(world, scene, entity, 0u);
    }
    for (auto& [guid, model] : m_Models)
    {
        model.DemandTree.clear();
        if (!model.Placements.empty())
            BuildPlacementDemandTree(model, 0, model.Placements.size());
    }
    m_PlacementMembers = std::move(members);
    if (extractionNeeded)
        m_RenderServices->RequestResidencyExtraction();
}

const Material* MeshImpostorSystem::UpdateModel(const GUID& model, ModelImpostor& impostor, ImpostorBaker& baker,
                                               ImpostorStage& stage, ForestPlacementFeature* forest, uint64 world,
                                               bool wantsForestAtlas, bool wantsForestShadowSources)
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
    request.Key.FrameSize = m_RenderServices->GetProfile().ImpostorFramePixels;
    request.Key.FilterMips = Platform::EnvironmentSwitchEnabled("GE_IMPOSTOR_FILTER_MIPS", false);
    request.Center = impostor.Center;
    request.Radius = impostor.Radius;
    const auto& materials = m_RenderServices->Materials().Registry();
    std::optional<ImpostorFoliageSway> sharedSway;
    for (const Part& part : impostor.Parts)
    {
        const Material* material = part.Mesh.IsValid() && !part.Material.IsNull() ? materials.Find(part.Material) : nullptr;
        if (!material || !CanBakeMaterial(*material))
        {
            UnlinkModel(impostor);
            return nullptr;
        }
        ImpostorFoliageSway sway{};
        if (material->GetCompileSpec().vertexModifierPath == "VertexModifiers/foliage_wind.glsl")
        {
            float motion[2]{};
            material->GetVector(HashStringId("foliageWindMotion"), motion, 2u);
            sway.MaxDisplacement = motion[0];
            if (sway.MaxDisplacement > 0.0f)
            {
                float range[2]{0.0f, 1.0f};
                material->GetVector(HashStringId("foliageWindHeight"), range, 2u);
                sway.BaseY = range[0];
                sway.Height = range[1];
            }
        }
        // Each source part contributes the same whole-tree sway.
        if (!std::isfinite(sway.BaseY) || !std::isfinite(sway.Height) || sway.Height <= 0.0f
            || !std::isfinite(sway.MaxDisplacement) || sway.MaxDisplacement < 0.0f
            || (sharedSway && *sharedSway != sway))
        {
            UnlinkModel(impostor);
            return nullptr;
        }
        sharedSway = sway;
        request.Key.BakeDiffuseTransmission |= Rendering::HasKeyword(
            material->GetVariantKey().materialKeywords, Rendering::MaterialKeyword::DiffuseTransmission);
        request.Key.MaterialHash = Hashing::HashCombine(request.Key.MaterialHash, material->GetBakeContentRevision());
        request.Key.MaterialHash = Hashing::HashCombine(request.Key.MaterialHash, material->GetVersion());
        request.Parts.push_back({part.Mesh, part.Material});
    }
    // Admit the bake only when every source mesh is drawable.
    const std::vector<uint32> meshes = ImpostorStage::PartMeshIndices(*m_RenderServices, PartMeshes(impostor.Parts));
    if (meshes.empty())
    {
        UnlinkModel(impostor);
        return nullptr;
    }
    const Mathematics::Vector4 bounds{impostor.Center.x, impostor.Center.y, impostor.Center.z, impostor.Radius};
    if (wantsForestShadowSources)
        forest->PublishShadowSources(world, model, bounds, request.Parts);
    if (!wantsForestAtlas && !impostor.WantsAtlas)
    {
        UnlinkModel(impostor);
        return nullptr;
    }

    // A request each frame keeps the atlas in the baker's least-recently-used budget.
    const ImpostorAtlas atlas = baker.Request(request, m_RenderServices->Spine().WorldFrameEpoch());
    const auto sway = sharedSway.value_or(ImpostorFoliageSway{});
    if (wantsForestAtlas)
    {
        const GUID packedGuid = GUID::Derive(model, "forest-impostor-" + std::to_string(materialIdentity));
        if (ImpostorStage::RegisterCardMaterial(*m_RenderServices, packedGuid, "Forest Impostor", atlas,
                impostor.Center, impostor.Radius, request.Key.FramesPerSide, kLayout, sway,
                ImpostorStage::CardMaterialStyle::PackedColor))
            forest->PublishCard(world, model, {packedGuid,
                bounds, sway, impostor.SwitchCoverage},
                request.Parts);
    }
    const bool useShadowCard = wantsForestAtlas && wantsForestShadowSources &&
                               ForestPlacementFeature::SunShadowCardsEnabled();
    static const bool coverageOnly = Platform::EnvironmentSwitchEnabled("GE_PACKED_FOREST_SHADOW_FAST_ALPHA", false);
    const bool useCoverageCard = useShadowCard && coverageOnly;
    if (useCoverageCard)
    {
        const GUID shadowGuid = GUID::Derive(model, "forest-shadow-impostor-" + std::to_string(materialIdentity));
        if (ImpostorStage::RegisterCardMaterial(*m_RenderServices, shadowGuid, "Forest Shadow Impostor", atlas,
                impostor.Center, impostor.Radius, request.Key.FramesPerSide, kLayout, sway,
                ImpostorStage::CardMaterialStyle::ShadowCoverage))
            forest->PublishSunShadowCard(world, model, shadowGuid);
    }
    if (!impostor.WantsAtlas && (!useShadowCard || useCoverageCard))
    {
        UnlinkModel(impostor);
        return nullptr;
    }
    const GUID cardGuid = GUID::Derive(model, "impostor-card-" + std::to_string(materialIdentity));
    const Material* card = ImpostorStage::RegisterCardMaterial(*m_RenderServices, cardGuid, "Impostor", atlas,
                                                               impostor.Center, impostor.Radius,
                                                               request.Key.FramesPerSide, kLayout,
                                                               sway);
    if (!card)
    {
        // Keep drawing the source while the atlas or base pipeline is unavailable.
        UnlinkModel(impostor);
        return nullptr;
    }
    if (useShadowCard && !useCoverageCard)
        forest->PublishSunShadowCard(world, model, cardGuid);
    if (!impostor.WantsAtlas)
    {
        UnlinkModel(impostor);
        return nullptr;
    }
    if (card->GetGpuSceneMaterialIndex() == impostor.LinkedMaterial && meshes == impostor.LinkedMeshes)
        return card;
    UnlinkModel(impostor);
    if (!stage.Link(*m_RenderServices, meshes, *card, impostor.SwitchCoverage, impostor.RequiresPlacement()))
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
