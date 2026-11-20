#include "EZTreeECS/Systems/EZTreeExtractionSystem.h"

#include "Components/Rendering/LocalBounds.h"
#include "Components/Rendering/MeshGPUData.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Rendering/WindVolume.h"
#include "Components/Transform.h"
#include "ECS/Components.h"
#include "ECS/ECSTemplates.h"
#include "ECS/WorldTemplateImplementations.inl"
#include "EZTree/EZTreeOptions.h"
#include "EZTreeECS/Components/EZTreeComponent.h"
#include "EZTreeECS/EZTreeService.h"
#include "EZTreeECS/EZTreeVariantMaterials.h"
#include "Engine/Rendering/Impostors/ImpostorBaker.h"
#include "Engine/Rendering/Impostors/ImpostorStage.h"
#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/ViewRegistry.h"
#include "Engine/Rendering/MaterialRegistry.h"
#include "Engine/Rendering/RenderOrigin.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/WorldDrawTypes.h"
#include "Rendering/Common/MatrixUtils.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/GPUScene.h"
#include "Rendering/Core/GPUInstanceWorldKey.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <vector>

namespace GameEngine::EZTreeECS
{
namespace
{

// ez_tree_wind.glsl's @deformationEnvelope: the largest displacement per unit of wind, local meters.
constexpr float kWindDisplacementEnvelope = 1.21f;

void WriteGuidToArray(const GUID& guid, std::array<uint8, 16>& out)
{
    std::copy(guid.GetData().begin(), guid.GetData().end(), out.begin());
}

// The strongest wind the volumes of `world` can add at any point, m/s. A volume blends
// toward its own speed or adds it, so the sum bounds the resolved wind over the base.
float WindVolumeSpeedBound(ECS::World& world)
{
    float bound = 0.0f;
    world.Query<ECS::Read<Components::WindVolume>>().Each(
        [&bound](ECS::EntityHandle, const Components::WindVolume& volume)
        {
            bound += std::max(volume.Speed, 0.0f) * std::clamp(volume.Weight, 0.0f, 1.0f);
        });
    return bound;
}

float LargestAxisScale(const Components::WorldTransform& worldTransform)
{
    return std::max({Mathematics::Vector3(worldTransform.Basis[0]).Length(),
                     Mathematics::Vector3(worldTransform.Basis[1]).Length(),
                     Mathematics::Vector3(worldTransform.Basis[2]).Length()});
}

// The culling sphere of a tree grows by the largest sway its wind can make, local meters.
// The GPU resolves the wind per instance, so the bound takes every volume at full strength.
float WindBoundsInflation(const EZTree::TreeOptions& options, float largestAxisScale, float volumeSpeedBound)
{
    const float scale = WindScaleBound(options);
    if (scale <= 0.0f)
        return 0.0f;
    return kWindDisplacementEnvelope * scale * (options.wind.strength.Length() * largestAxisScale + volumeSpeedBound);
}

void SubmitPart(Engine::Renderer::RenderServices& renderServices,
                ECS::World& world,
                [[maybe_unused]] ECS::EntityHandle entity,
                const Components::WorldTransform& worldTransform,
                const Components::LocalBounds& bounds,
                Rendering::MeshGPUHandle meshHandle,
                const GUID& materialGuid,
                float boundsInflation,
                uint32& instanceIndex)
{
    if (!meshHandle.IsValid())
        return;

    auto* scene = renderServices.GetGPUScene();
    if (!scene)
        return;

    const auto* material = renderServices.Materials().Registry().Find(materialGuid);
    if (!material || !material->GetGraphicsPipelineId().IsValid())
        return;

    const auto* meshEntry = renderServices.GetMeshGPURegistry().Find(meshHandle);
    if (!meshEntry)
        return;

    const uint32 meshIndex = meshEntry->gpuMeshIndex;
    const uint32 materialIndex = material->GetGpuSceneMaterialIndex();
    if (meshIndex == 0xFFFFFFFFu || materialIndex == 0xFFFFFFFFu)
        return;

    Mathematics::Vector3Int sector;
    const Mathematics::Matrix4x4 localMatrix = GameEngine::Engine::Renderer::SectorLocalMatrix(worldTransform, sector);
    const float* elements = localMatrix.Data();
    Rendering::GPUInstance instance{};
    std::memcpy(instance.transform.Data(), elements, sizeof(float) * 16);
    std::memcpy(instance.prevTransform.Data(), elements, sizeof(float) * 16);
    GameEngine::Rendering::PackSector(sector.X, sector.Y, sector.Z, instance.sectorPacked[0], instance.sectorPacked[1]);
    bool mirrored = false;
    Rendering::Vector4 normalColumn0;
    Rendering::MatrixUtils::ComputeNormalMatrixColumns(
        elements,
        normalColumn0,
        instance.normalMatrixCol1,
        instance.normalMatrixCol2,
        &mirrored);
    instance.normalMatrixCol0 = Rendering::Vector3(normalColumn0.x, normalColumn0.y, normalColumn0.z);
    // EZTree draws on the default layer (layer 0), shared by CPU submission and GPU culling.
    constexpr Rendering::RenderLayerMask kRenderLayers = Rendering::RenderLayerMask::Default();
    instance.renderLayerMask = kRenderLayers.Bits;
    instance.meshIndex = meshIndex;
    instance.materialIndex = materialIndex;
    const uint64 worldId = world.GetWorldId();
    const uint32 worldKey16 = Rendering::PackWorldKey16(worldId);
    // Keep the custom GPU-scene path in lockstep with the normal render
    // extraction path. Shadow scatter uses these depth-class bits to route
    // material-dependent casters (alpha test / vertex modifiers) and
    // double-sided casters to the matching shadow batch; the mirror bit splits
    // negative-determinant instances so their winding is compensated.
    instance.flags = 1u | 2u
        | Rendering::DepthClassInstanceFlagBits(
            renderServices.Materials().GetMaterialDepthClass(materialIndex))
        | (worldKey16 << 16);
    if (mirrored && Rendering::MirroredWindingEnabled())
        instance.flags |= Rendering::kInstanceFlagMirrored;
    instance.lodBias = 0.0f;
    instance.custom0 = Rendering::Vector4(1.0f, 1.0f, 1.0f, 1.0f);
    // The bounding center is local to the sector in sectorPacked; each reader composes it.
    instance.boundingCenter = localMatrix.TransformPoint(bounds.Box.center);
    const float sx = std::sqrt(elements[0] * elements[0] + elements[1] * elements[1] + elements[2] * elements[2]);
    const float sy = std::sqrt(elements[4] * elements[4] + elements[5] * elements[5] + elements[6] * elements[6]);
    const float sz = std::sqrt(elements[8] * elements[8] + elements[9] * elements[9] + elements[10] * elements[10]);
    instance.boundingRadius = (bounds.Box.Radius() + boundsInflation) * std::max({sx, sy, sz});

    if (instanceIndex == 0xFFFFFFFFu)
        instanceIndex = scene->AddInstance(instance);
    else
        scene->UpdateInstance(instanceIndex, instance);

    const auto& views = renderServices.Views().GetViews();
    if (views.empty())
        return;

    std::vector<Engine::Renderer::WorldSubmissionRecord> submissions;
    submissions.reserve(views.size());
    for (const auto& view : views)
    {
        if (view.worldId != 0 && view.worldId != world.GetWorldId())
            continue;
        if (!kRenderLayers.Intersects(view.ActiveRenderLayerMask()))
            continue;

        Engine::Renderer::WorldSubmissionRecord sub{};
        sub.viewId = view.id;
        sub.meshHandle = meshHandle;
        sub.material = material;
        sub.instanceIndex = instanceIndex;
        sub.renderLayerMask = kRenderLayers;
        sub.flags = instance.flags;
        submissions.push_back(sub);
    }
    if (!submissions.empty())
        renderServices.SubmitWorldSubmissions(submissions);
}


void RemoveInstance(Rendering::GPUScene* scene, uint32& index)
{
    if (index == 0xFFFFFFFFu)
        return;
    if (scene)
        scene->RemoveInstance(index);
    index = 0xFFFFFFFFu;
}

void ReleasePartInstances(Engine::Renderer::RenderServices& renderServices, Components::EZTree& tree)
{
    auto* scene = renderServices.GetGPUScene();
    RemoveInstance(scene, tree.RuntimeBranchInstanceIndex);
    RemoveInstance(scene, tree.RuntimeLeafInstanceIndex);
    RemoveInstance(scene, tree.RuntimeTrellisInstanceIndex);
}

std::array<uint32, 3> PartInstances(const Components::EZTree& tree)
{
    return {tree.RuntimeBranchInstanceIndex, tree.RuntimeLeafInstanceIndex, tree.RuntimeTrellisInstanceIndex};
}

// The parts of a variant that the impostor stage links to its card, the primary part first.
std::vector<Rendering::MeshGPUHandle> ImpostorParts(const TreeVariant& variant)
{
    std::vector<Rendering::MeshGPUHandle> parts;
    for (const Rendering::MeshGPUHandle part : {variant.Branches, variant.Leaves, variant.Trellis})
    {
        if (part.IsValid())
            parts.push_back(part);
    }
    return parts;
}

Engine::Renderer::ImpostorBakeRequest MakeImpostorBakeRequest(const TreeVariant& variant)
{
    Engine::Renderer::ImpostorBakeRequest request{};
    // The options hash fixes the meshes and the materials alike.
    request.Key.MeshHash = variant.Hash;
    request.Key.MaterialHash = variant.Hash;
    request.Key.Layout = Mathematics::OctahedralLayout::Hemisphere;
    request.Center = variant.Bounds.center;
    request.Radius = variant.ImpostorRadius;
    if (variant.Branches.IsValid())
        request.Parts.push_back({variant.Branches, variant.Materials.BarkStatic});
    if (variant.Leaves.IsValid())
        request.Parts.push_back({variant.Leaves, variant.Materials.LeavesStatic});
    if (variant.Trellis.IsValid())
        request.Parts.push_back({variant.Trellis, variant.Materials.Trellis});
    return request;
}

void WriteVariantToTree(const TreeVariant& variant, Components::EZTree& tree)
{
    tree.RuntimeOptionsHash = variant.Hash;
    tree.RuntimeMeshHandleId = static_cast<uint64>(variant.Combined);
    tree.RuntimeBranchMeshHandleId = static_cast<uint64>(variant.Branches);
    tree.RuntimeLeafMeshHandleId = static_cast<uint64>(variant.Leaves);
    tree.RuntimeTrellisMeshHandleId = static_cast<uint64>(variant.Trellis);
    WriteGuidToArray(variant.MeshKey, tree.RuntimeMeshGuid);
    tree.RuntimeVertexCount = variant.VertexCount;
    tree.RuntimeTriangleCount = variant.TriangleCount;
    tree.RuntimeBranchCount = variant.BranchCount;
}

} // namespace

struct EZTreeExtractionSystem::FrameContext
{
    ECS::World& World;
    EZTreeService& Service;
    uint64 WorldId = 0;
    float VolumeSpeedBound = 0.0f;
    bool DeviceRebuilt = false;
    // The variants this pass has read, so each one is copied out of the service once.
    FlatMap<uint64, TreeVariant> Variants;
};

EZTreeExtractionSystem::~EZTreeExtractionSystem()
{
    auto* service = EZTreeService::TryGet();
    if (!service)
        return;
    for (const auto& [key, binding] : m_Bindings)
        service->ReleaseVariant(binding.Hash);
}

void EZTreeExtractionSystem::Update(ECS::World& world, float32 /*deltaTime*/)
{
    if (!m_RenderServices)
        return;

    auto* service = EZTreeService::TryGet();
    if (!service)
        return;

    // This system runs on a job worker: its wave holds several systems, and at
    // least one of them (MorphTarget) registers meshes too. Variant generation
    // registers four submeshes and SubmitPart dereferences a Find() pointer, so
    // the exclusion spans the whole pass, not each call.
    Rendering::MeshGPURegistry::TableScope registryScope(
        m_RenderServices->GetMeshGPURegistry());

    // An in-place device rebuild freed every submesh. The service regenerates the
    // variants; the trees drop their GPU instances and add them again below.
    const uint64 deviceGeneration = m_RenderServices->GetDevice()
        ? m_RenderServices->GetDevice()->GetDeviceRebuildGeneration() : 0u;
    const bool deviceRebuilt = deviceGeneration != m_LastDeviceRebuildGeneration && deviceGeneration != 0u;
    m_LastDeviceRebuildGeneration = deviceGeneration;
    service->Refresh(deviceGeneration);

    ++m_UpdateStamp;
    FrameContext frame{world, *service, world.GetWorldId(), WindVolumeSpeedBound(world), deviceRebuilt, {}};
    // The rebuild freed the variant meshes with their impostor links.
    if (deviceRebuilt)
        m_ImpostorLinks.Clear();

    // Every tree is visited whatever its enable state: one that is switched off,
    // or whose entity is, releases the GPU instances this system gave it.
    world.Query<ECS::Write<Components::EZTree>,
                ECS::Optional<ECS::Disabled>,
                ECS::Optional<ECS::DisabledInHierarchy>,
                ECS::Optional<ECS::ComponentDisabled<Components::EZTree>>>()
        .IncludeDisabled()
        .Each([this, &frame](ECS::EntityHandle entity, Components::EZTree& tree, const ECS::Disabled* disabled,
                             const ECS::DisabledInHierarchy* inactive,
                             const ECS::ComponentDisabled<Components::EZTree>* treeOff)
        { ExtractTree(frame, entity, tree, !disabled && !inactive && !treeOff); });

    ReleaseUnseenTrees(frame.WorldId, *service);

    RequestImpostors(frame);
}

void EZTreeExtractionSystem::RequestImpostors(FrameContext& frame)
{
    // A variant this pass did not read is released: its meshes, and their links, are gone.
    m_ImpostorLinks.EraseIf([&frame](uint64 hash, uint32) { return frame.Variants.Find(hash) == nullptr; });
    if (frame.Variants.Empty())
        return;
    auto& stage = m_RenderServices->EnsureFeature<Engine::Renderer::ImpostorStage>();
    stage.EnsureCardMesh(*m_RenderServices);
    auto& baker = m_RenderServices->EnsureFeature<Engine::Renderer::ImpostorBaker>();
    for (const auto& [hash, variant] : frame.Variants)
    {
        // A request each frame keeps the atlas in the baker's least-recently-used budget.
        const Engine::Renderer::ImpostorAtlas atlas = baker.Request(MakeImpostorBakeRequest(variant), m_UpdateStamp);
        const Engine::Renderer::Material* card = Engine::Renderer::ImpostorStage::RegisterCardMaterial(
            *m_RenderServices, variant.Materials.Impostor, "Tree Generator Impostor", atlas, variant.Bounds.center,
            variant.ImpostorRadius, Engine::Renderer::ImpostorAtlasKey{}.FramesPerSide,
            Mathematics::OctahedralLayout::Hemisphere);
        const std::vector<Rendering::MeshGPUHandle> parts = ImpostorParts(variant);
        uint32& linked = m_ImpostorLinks.GetOrInsert(hash);
        if (!card)
        {
            // Not baked, evicted, or the card pipeline is still building: the trees draw their meshes.
            if (linked != 0u)
                Engine::Renderer::ImpostorStage::Unlink(*m_RenderServices, parts);
            linked = 0u;
            continue;
        }
        // The link holds the card material index + 1, so 0 means not linked.
        const uint32 cardKey = card->GetGpuSceneMaterialIndex() + 1u;
        if (linked != cardKey)
            linked = stage.Link(*m_RenderServices, parts, *card) ? cardKey : 0u;
    }
}

void EZTreeExtractionSystem::ExtractTree(FrameContext& frame,
                                         ECS::EntityHandle entity,
                                         Components::EZTree& tree,
                                         bool active)
{
    // Deferred, as every structural change here: an immediate one moves the entity to
    // another archetype under the query and leaves `tree` pointing at the old row.
    if (frame.World.GetComponent<Components::MeshRenderer>(entity))
        frame.World.RemoveComponent<Components::MeshRenderer>(entity);
    if (frame.World.GetComponent<Components::MeshGPUData>(entity))
        frame.World.RemoveComponent<Components::MeshGPUData>(entity);

    const uint64 hash = EZTree::HashOptions(tree.Options) ^ static_cast<uint64>(tree.RandomizeCounter);
    TreeBinding* binding = m_Bindings.Find(BindingKey{frame.WorldId, entity.id});
    if (!binding)
    {
        // A tree this system has not seen holds no variant reference and no GPU
        // instance of this system, whatever its runtime fields say: they may be
        // bytes copied from another world.
        ReleaseVariantFields(tree);
        binding = &m_Bindings.InsertOrAssign(BindingKey{frame.WorldId, entity.id}, TreeBinding{});
    }
    binding->Stamp = m_UpdateStamp;
    if (frame.DeviceRebuilt)
    {
        ReleasePartInstances(*m_RenderServices, tree);
        binding->Instances = PartInstances(tree);
    }

    if (binding->Hash != hash)
    {
        if (binding->Hash != 0)
            frame.Service.ReleaseVariant(binding->Hash);
        const TreeVariant acquired = frame.Service.AcquireVariant(hash, tree.Options);
        binding->Hash = hash;
        frame.Variants.InsertOrAssign(hash, acquired);
        Components::LocalBounds bounds{};
        bounds.Box = acquired.Bounds;
        frame.World.AddComponent(entity, bounds);
    }
    else if (tree.RuntimeOptionsHash == 0)
    {
        // The editor marks a tree dirty with a zero hash. Equal options keep the
        // variant; the materials register again for edited material assets.
        frame.Service.InvalidateMaterials(hash);
    }

    const TreeVariant* variant = frame.Variants.Find(hash);
    if (!variant)
        variant = &frame.Variants.InsertOrAssign(hash, frame.Service.FindVariant(hash));
    WriteVariantToTree(*variant, tree);

    if (!active)
    {
        ReleasePartInstances(*m_RenderServices, tree);
        binding->Instances = PartInstances(tree);
        return;
    }

    const auto* worldTransform = frame.World.GetComponent<Components::WorldTransform>(entity);
    if (!worldTransform)
        return;

    // Every part has the same bounds and inflation, so every part has the same instance sphere and
    // the scatter switches all parts of a tree to its impostor card in the same frame.
    Components::LocalBounds bounds{};
    bounds.Box = variant->Bounds;
    const float inflation =
        WindBoundsInflation(tree.Options, LargestAxisScale(*worldTransform), frame.VolumeSpeedBound);
    SubmitPart(*m_RenderServices, frame.World, entity, *worldTransform, bounds, variant->Branches,
               SelectBarkMaterial(*m_RenderServices, variant->Materials), inflation,
               tree.RuntimeBranchInstanceIndex);
    SubmitPart(*m_RenderServices, frame.World, entity, *worldTransform, bounds, variant->Leaves,
               SelectLeafMaterial(*m_RenderServices, variant->Materials), inflation,
               tree.RuntimeLeafInstanceIndex);
    SubmitPart(*m_RenderServices, frame.World, entity, *worldTransform, bounds, variant->Trellis,
               variant->Materials.Trellis, inflation, tree.RuntimeTrellisInstanceIndex);
    binding->Instances = PartInstances(tree);
}

void EZTreeExtractionSystem::ReleaseVariantFields(Components::EZTree& tree)
{
    tree.RuntimeOptionsHash = 0;
    tree.RuntimeBranchInstanceIndex = 0xFFFFFFFFu;
    tree.RuntimeLeafInstanceIndex = 0xFFFFFFFFu;
    tree.RuntimeTrellisInstanceIndex = 0xFFFFFFFFu;
}

void EZTreeExtractionSystem::ReleaseUnseenTrees(uint64 worldId, EZTreeService& service)
{
    // A tree the query did not visit this pass was destroyed or lost its component.
    auto* scene = m_RenderServices->GetGPUScene();
    m_Bindings.EraseIf([this, worldId, scene, &service](const BindingKey& key, TreeBinding& binding)
    {
        if (key.first != worldId || binding.Stamp == m_UpdateStamp)
            return false;
        for (uint32& index : binding.Instances)
            RemoveInstance(scene, index);
        if (binding.Hash != 0)
            service.ReleaseVariant(binding.Hash);
        return true;
    });
}

} // namespace GameEngine::EZTreeECS
