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
#include "Engine/Rendering/Material.h"
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
    const Mathematics::Matrix4x4 basis = worldTransform.ToMatrix4x4(Mathematics::Vector3{});
    const float* m = basis.Data();
    const float sx = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
    const float sy = std::sqrt(m[4] * m[4] + m[5] * m[5] + m[6] * m[6]);
    const float sz = std::sqrt(m[8] * m[8] + m[9] * m[9] + m[10] * m[10]);
    return std::max({sx, sy, sz});
}

// The culling sphere of a tree grows by the largest sway its wind can make, local meters.
// The GPU resolves the wind per instance, so the bound takes every volume at full strength.
float WindBoundsInflation(const EZTree::TreeOptions& options, float largestAxisScale, float volumeSpeedBound)
{
    const float scale = WindScaleBound(options);
    if (scale <= 0.0f)
        return 0.0f;
    const float authored = std::sqrt(options.wind.strength.x * options.wind.strength.x +
                                     options.wind.strength.y * options.wind.strength.y +
                                     options.wind.strength.z * options.wind.strength.z);
    return kWindDisplacementEnvelope * scale * (authored * largestAxisScale + volumeSpeedBound);
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
    const uint64 worldId = world.GetWorldId();
    const float volumeSpeedBound = WindVolumeSpeedBound(world);
    FlatMap<uint64, TreeVariant> frameVariants;

    // Every tree is visited whatever its enable state: one that is switched off,
    // or whose entity is, releases the GPU instances this system gave it.
    world.Query<ECS::Write<Components::EZTree>,
                ECS::Optional<ECS::Disabled>,
                ECS::Optional<ECS::DisabledInHierarchy>,
                ECS::Optional<ECS::ComponentDisabled<Components::EZTree>>>()
        .IncludeDisabled()
        .Each([&](ECS::EntityHandle entity, Components::EZTree& tree, const ECS::Disabled* disabled,
                  const ECS::DisabledInHierarchy* inactive, const ECS::ComponentDisabled<Components::EZTree>* treeOff)
        {
            if (world.GetComponent<Components::MeshRenderer>(entity))
                world.RemoveComponent<Components::MeshRenderer>(entity);
            if (world.GetComponent<Components::MeshGPUData>(entity))
                world.RemoveComponent<Components::MeshGPUData>(entity);

            const uint64 hash = EZTree::HashOptions(tree.Options) ^ static_cast<uint64>(tree.RandomizeCounter);
            TreeBinding* binding = m_Bindings.Find(BindingKey{worldId, entity.id});
            if (!binding)
            {
                // A tree this system has not seen holds no variant reference and no GPU
                // instance of this system, whatever its runtime fields say: they may be
                // bytes copied from another world.
                ReleaseVariantFields(tree);
                binding = &m_Bindings.InsertOrAssign(BindingKey{worldId, entity.id}, TreeBinding{});
            }
            binding->Stamp = m_UpdateStamp;
            if (deviceRebuilt)
            {
                ReleasePartInstances(*m_RenderServices, tree);
                binding->Instances = PartInstances(tree);
            }

            if (binding->Hash != hash)
            {
                if (binding->Hash != 0)
                    service->ReleaseVariant(binding->Hash);
                const TreeVariant variant = service->AcquireVariant(hash, tree.Options);
                binding->Hash = hash;
                frameVariants.InsertOrAssign(hash, variant);
                // Deferred: an immediate add moves the entity to another archetype
                // under this query and leaves `tree` pointing at the old row.
                Components::LocalBounds bounds{};
                bounds.Box = variant.Bounds;
                world.AddComponent(entity, bounds);
            }
            else if (tree.RuntimeOptionsHash == 0)
            {
                // The editor marks a tree dirty with a zero hash. Equal options keep the
                // variant; the materials register again for edited material assets.
                service->InvalidateMaterials(hash);
            }

            const TreeVariant* variant = frameVariants.Find(hash);
            if (!variant)
                variant = &frameVariants.InsertOrAssign(hash, service->FindVariant(hash));
            WriteVariantToTree(*variant, tree);

            if (disabled || inactive || treeOff)
            {
                ReleasePartInstances(*m_RenderServices, tree);
                binding->Instances = PartInstances(tree);
                return;
            }

            const auto* worldTransform = world.GetComponent<Components::WorldTransform>(entity);
            if (!worldTransform)
                return;

            Components::LocalBounds bounds{};
            bounds.Box = variant->Bounds;
            const float inflation =
                WindBoundsInflation(tree.Options, LargestAxisScale(*worldTransform), volumeSpeedBound);
            SubmitPart(*m_RenderServices, world, entity, *worldTransform, bounds, variant->Branches,
                       SelectBarkMaterial(*m_RenderServices, variant->Materials), inflation,
                       tree.RuntimeBranchInstanceIndex);
            SubmitPart(*m_RenderServices, world, entity, *worldTransform, bounds, variant->Leaves,
                       SelectLeafMaterial(*m_RenderServices, variant->Materials), inflation,
                       tree.RuntimeLeafInstanceIndex);
            SubmitPart(*m_RenderServices, world, entity, *worldTransform, bounds, variant->Trellis,
                       variant->Materials.Trellis, inflation, tree.RuntimeTrellisInstanceIndex);
            binding->Instances = PartInstances(tree);
        });

    ReleaseUnseenTrees(worldId, *service);
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
    m_Bindings.EraseIf([&](const BindingKey& key, TreeBinding& binding)
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
