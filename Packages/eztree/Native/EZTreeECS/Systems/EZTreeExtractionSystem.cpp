#include "EZTreeECS/Systems/EZTreeExtractionSystem.h"

#include "Assets/HlodSelect.h"
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
#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/PrimitiveGenerator.h"
#include "Engine/Rendering/ViewRegistry.h"
#include "Platform/Environment.h"
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
                uint32& instanceIndex,
                const Rendering::Vector4& custom0 = Rendering::Vector4(1.0f, 1.0f, 1.0f, 1.0f))
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
    instance.custom0 = custom0;
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

void ReleaseMeshInstances(Engine::Renderer::RenderServices& renderServices, Components::EZTree& tree)
{
    auto* scene = renderServices.GetGPUScene();
    RemoveInstance(scene, tree.RuntimeBranchInstanceIndex);
    RemoveInstance(scene, tree.RuntimeLeafInstanceIndex);
    RemoveInstance(scene, tree.RuntimeTrellisInstanceIndex);
}

void ReleasePartInstances(Engine::Renderer::RenderServices& renderServices, Components::EZTree& tree)
{
    ReleaseMeshInstances(renderServices, tree);
    RemoveInstance(renderServices.GetGPUScene(), tree.RuntimeImpostorInstanceIndex);
}

std::array<uint32, 4> PartInstances(const Components::EZTree& tree)
{
    return {tree.RuntimeBranchInstanceIndex, tree.RuntimeLeafInstanceIndex, tree.RuntimeTrellisInstanceIndex,
            tree.RuntimeImpostorInstanceIndex};
}

// The tree's impostor sphere in world space.
void WorldSphere(const Components::WorldTransform& worldTransform, const Mathematics::Vector3& localCenter,
                 float localRadius, float (&outCenter)[3], float& outRadius)
{
    const Mathematics::Vector3 rotated = Mathematics::Vector3(worldTransform.Basis[0]) * localCenter.x +
                                         Mathematics::Vector3(worldTransform.Basis[1]) * localCenter.y +
                                         Mathematics::Vector3(worldTransform.Basis[2]) * localCenter.z;
    const Mathematics::Vector3 center = (worldTransform.GetPosition() - Mathematics::WorldPosition{}) + rotated;
    outCenter[0] = center.x;
    outCenter[1] = center.y;
    outCenter[2] = center.z;
    outRadius = localRadius * LargestAxisScale(worldTransform);
}

// custom0 of an impostor card: xy the hemi-octahedral code of the direction from the tree toward
// the camera in the tree's own frame, zw the cosine and sine of the tree's yaw (impostor.glsl).
Rendering::Vector4 ImpostorCardData(const Components::WorldTransform& worldTransform, const float (&center)[3],
                                    const float (&camera)[3])
{
    const Mathematics::Vector3 axisX = Mathematics::Vector3(worldTransform.Basis[0]).Normalize();
    const Mathematics::Vector3 axisY = Mathematics::Vector3(worldTransform.Basis[1]).Normalize();
    const Mathematics::Vector3 axisZ = Mathematics::Vector3(worldTransform.Basis[2]).Normalize();
    const Mathematics::Vector3 toCamera =
        Mathematics::Vector3(camera[0] - center[0], camera[1] - center[1], camera[2] - center[2]);
    const Mathematics::Vector3 local(Mathematics::Vector3::Dot(toCamera, axisX), Mathematics::Vector3::Dot(toCamera, axisY),
                                     Mathematics::Vector3::Dot(toCamera, axisZ));
    const Mathematics::Vector2 code = Mathematics::HemiOctahedralEncode(local.NormalizeOr(Mathematics::Vector3(0.0f, 1.0f, 0.0f)));
    const float yawLength = std::max(std::sqrt(axisX.x * axisX.x + axisX.z * axisX.z), 1e-6f);
    return Rendering::Vector4(code.x, code.y, axisX.x / yawLength, -axisX.z / yawLength);
}

// Screen coverage (Hlod::ClusterCoverage: radius * proj[1][1] / distance, with the impostor's
// sphere) under which a tree draws its impostor. A frame of the default atlas key spans the sphere
// over 128 pixels, so at 1080 lines this is about one atlas texel per screen pixel.
constexpr float kImpostorSwitchCoverage = 0.12f;
constexpr float kImpostorSwitchHysteresis = 0.15f;
// A tree requests its atlas a little before it switches, so the bake is ready when it is needed.
constexpr float kImpostorBakeCoverage = 1.5f * kImpostorSwitchCoverage;

Engine::Renderer::ImpostorBakeRequest MakeImpostorBakeRequest(const TreeVariant& variant)
{
    Engine::Renderer::ImpostorBakeRequest request{};
    // The options hash fixes the meshes and the materials alike.
    request.Key.MeshHash = variant.Hash;
    request.Key.MaterialHash = variant.Hash;
    request.Key.Layout = Mathematics::OctahedralLayout::Hemisphere;
    request.Key.FilterMips = Platform::EnvironmentSwitchEnabled("GE_IMPOSTOR_FILTER_MIPS", false);
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
    // The viewpoint camera the impostor switch measures coverage from; no camera keeps every
    // tree on its mesh.
    Hlod::ViewLod View;
    Hlod::SwitchConfig ImpostorSwitch;
    Rendering::MeshGPUHandle ImpostorCard;
    // Variants with a tree near or past the switch: their atlases are requested after the pass.
    FlatMap<uint64, bool> WantedImpostors;
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
    FrameContext frame{world, *service, world.GetWorldId(), WindVolumeSpeedBound(world), deviceRebuilt, {}, {}, {}, {}, {}};
    frame.ImpostorSwitch = Hlod::MakeSwitchConfig(kImpostorSwitchCoverage, kImpostorSwitchHysteresis);
    // GE_IMPOSTORS=0 keeps every tree on its mesh: the reference for comparisons, as GE_HLOD=0 is.
    static const bool impostorsEnabled = Platform::EnvironmentSwitchEnabled("GE_IMPOSTORS", true);
    if (const Rendering::CameraData* camera = impostorsEnabled
            ? m_RenderServices->Views().FindCameraData(m_RenderServices->Views().GetViewpointCamera())
            : nullptr)
    {
        frame.View.CameraPos[0] = camera->cameraPos.x;
        frame.View.CameraPos[1] = camera->cameraPos.y;
        frame.View.CameraPos[2] = camera->cameraPos.z;
        // An orthographic or 2D view has no distance-based switch.
        frame.View.ProjScaleY = camera->cameraPos.w == 1.0f ? 0.0f : std::abs(camera->proj[1][1]);
    }
    frame.ImpostorCard = m_RenderServices->GetMeshGPURegistry().RegisterSubmesh(
        Rendering::MeshGPUKey{Engine::Renderer::PrimitiveGenerator::PlaneSpriteUvGuid(), 0u},
        Engine::Renderer::PrimitiveGenerator::GeneratePlaneSpriteUv());

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
    auto& baker = m_RenderServices->EnsureFeature<Engine::Renderer::ImpostorBaker>();
    for (const auto& [hash, wanted] : frame.WantedImpostors)
    {
        const TreeVariant* variant = frame.Variants.Find(hash);
        if (!variant)
            continue;
        const Engine::Renderer::ImpostorAtlas atlas = baker.Request(MakeImpostorBakeRequest(*variant), m_UpdateStamp);
        ImpostorMaterial* material = m_ImpostorMaterials.Find(hash);
        if (!material)
            material = &m_ImpostorMaterials.InsertOrAssign(hash, ImpostorMaterial{});
        const bool atlasReady = atlas.Color.IsValid() && atlas.NormalDepth.IsValid();
        if (atlasReady && material->Color != atlas.Color)
        {
            RegisterImpostorMaterial(*m_RenderServices, variant->Materials.Impostor, atlas, variant->Bounds.center,
                                     variant->ImpostorRadius,
                                     Engine::Renderer::ImpostorAtlasKey{}.FramesPerSide,
                                     Mathematics::OctahedralLayout::Hemisphere);
            material->Color = atlas.Color;
        }
        // A card whose pipeline is not built yet would draw nothing: the tree keeps its mesh until then.
        const Engine::Renderer::Material* card =
            atlasReady ? m_RenderServices->Materials().Registry().Find(variant->Materials.Impostor) : nullptr;
        material->Ready = card && card->GetGraphicsPipelineId().IsValid();
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

    Components::LocalBounds bounds{};
    bounds.Box = variant->Bounds;

    float center[3];
    float radius = 0.0f;
    WorldSphere(*worldTransform, variant->Bounds.center, variant->ImpostorRadius, center, radius);
    const float coverage = Hlod::ClusterCoverage(frame.View, center, radius);
    if (coverage < kImpostorBakeCoverage)
        frame.WantedImpostors.InsertOrAssign(hash, true);
    binding->Impostor = Hlod::UpdateProxyActive(binding->Impostor, coverage, frame.ImpostorSwitch);
    const ImpostorMaterial* impostor = m_ImpostorMaterials.Find(hash);
    if (binding->Impostor && impostor && impostor->Ready)
    {
        ReleaseMeshInstances(*m_RenderServices, tree);
        SubmitPart(*m_RenderServices, frame.World, entity, *worldTransform, bounds, frame.ImpostorCard,
                   variant->Materials.Impostor, 0.0f, tree.RuntimeImpostorInstanceIndex,
                   ImpostorCardData(*worldTransform, center, frame.View.CameraPos));
        binding->Instances = PartInstances(tree);
        return;
    }
    RemoveInstance(m_RenderServices->GetGPUScene(), tree.RuntimeImpostorInstanceIndex);

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
    tree.RuntimeImpostorInstanceIndex = 0xFFFFFFFFu;
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
