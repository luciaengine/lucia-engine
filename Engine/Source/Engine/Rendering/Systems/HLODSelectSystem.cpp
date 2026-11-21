#include "ECSModules/Rendering/Systems/HLODSelectSystem.h"

#include "Assets/AssetManager.h"
#include "Assets/HlodSelect.h"
#include "Components/Rendering/HLODVolume.h"
#include "Components/Rendering/LODGroup.h"
#include "Components/Rendering/LocalBounds.h"
#include "Components/Rendering/MeshGPUData.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Transform.h"
#include "Core/Engine.h"
#include "Engine/Rendering/HlodRuntime.h"
#include "Engine/Rendering/MeshLODThresholds.h"
#include "Engine/Rendering/RenderOrigin.h"
#include "Engine/Rendering/RenderServices.h"
#include "Logger/Logger.h"
#include "Platform/Environment.h"
#include "Rendering/Camera/CameraTypes.h"
#include "Rendering/Common/MatrixUtils.h"
#include "Rendering/Core/GPUScene.h"

#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <mutex>

namespace GameEngine {
namespace Engine::Renderer {

namespace {

// GE_HLOD=0 disables the runtime switch entirely (zero overhead); unset keeps
// it on. Read once.
bool IsHlodRuntimeEnabled() {
    static bool s_Enabled = Platform::EnvironmentSwitchEnabled("GE_HLOD", true);
    return s_Enabled;
}

constexpr uint32 kUnassignedSlot = 0xFFFFFFFFu;
constexpr uint32 kAutomaticLod = 0xFFFFFFFFu;

void EvictOne(Components::MeshGPUData& gpu) {
    gpu.instanceIndex = kUnassignedSlot;
    gpu.meshIndex = kUnassignedSlot;
    gpu.materialIndex = kUnassignedSlot;
    gpu.hlodEvicted = true;
}

bool AllProxiesDrawable(const ECS::World& world, const Rendering::MeshGPURegistry& registry,
                        const Hlod::RuntimeCluster& cluster)
{
    Rendering::MeshGPURegistry::TableScope scope(registry);
    return !cluster.Proxies.empty() && std::ranges::all_of(cluster.Proxies, [&](ECS::EntityHandle proxy)
    {
        const auto* renderer = world.GetComponent<Components::MeshRenderer>(proxy);
        if (!renderer)
            return false;
        const auto* mesh = registry.Find(Rendering::MeshGPUHandle(renderer->meshGpuHandleId));
        return mesh && registry.IsEntryDrawable(*mesh);
    });
}

bool RendererChanged(const Components::MeshRenderer& renderer, const Hlod::RuntimeCluster& cluster,
                     size_t memberIndex)
{
    return Hlod::MemberMeshSource::Of(renderer) != cluster.MemberMeshSources[memberIndex] ||
           renderer.materialAssetGuid.ToGuid() != cluster.MemberMaterialGuid[memberIndex] ||
           renderer.RenderLayers != cluster.RenderLayers;
}

bool HasSplitMember(const ECS::World& world, const Hlod::RuntimeCluster& cluster)
{
    return std::ranges::any_of(cluster.Members, [&](ECS::EntityHandle member)
    {
        const auto* gpu = world.GetComponent<Components::MeshGPUData>(member);
        return gpu && gpu->clusterSplitOwner;
    });
}

void RebuildMemberEnvelope(const ECS::World& world, const Rendering::MeshGPURegistry& registry,
                           const Rendering::GPUScene& scene, const Hlod::RuntimeCluster& cluster,
                           Hlod::RuntimeCluster::LodEnvelope& envelope,
                           uint64 meshRevision, uint64 inputRevision,
                           float defaultScale, float tightScale)
{
    envelope = {meshRevision, inputRevision, defaultScale, tightScale, 0.0f, 0.0f, true};
    const Mathematics::WorldPosition clusterCenter{cluster.SphereCenter[0], cluster.SphereCenter[1],
                                                    cluster.SphereCenter[2]};
    const auto& meshRows = scene.GetMeshes();
    Rendering::MeshGPURegistry::TableScope scope(registry);
    for (const auto& member : cluster.CoarsenedMembers)
    {
        const auto* mesh = registry.FindByKey(member.Mesh);
        const auto* transform = world.GetComponent<Components::WorldTransform>(member.Entity);
        if (!mesh || mesh->gpuMeshIndex >= meshRows.size() || !transform)
        {
            envelope.RequiredDistanceAtUnitProjection = std::numeric_limits<float>::infinity();
            return;
        }
        const auto& row = meshRows[mesh->gpuMeshIndex];
        if (member.BakedLevel + 1u != row.lodCount || member.BakedLevel >= Rendering::kMaxMeshLODs)
        {
            envelope.RequiredDistanceAtUnitProjection = std::numeric_limits<float>::infinity();
            return;
        }
        const float sseScale = (row.lodFlags & Rendering::kGPUMeshLodTightClassBit) != 0
            ? tightScale : defaultScale;
        float threshold = Rendering::kLodEffNone;
        for (uint32 level = 0; level < member.BakedLevel; ++level)
        {
            threshold = Rendering::LodEffectiveThreshold(row.lodThreshold[level],
                (row.lodFlags & (1u << level)) != 0, sseScale, row.lodSseScaleCeil, threshold);
        }

        const Mathematics::Vector3 offset = transform->GetPosition() - clusterCenter;
        const auto* bounds = world.GetComponent<Components::LocalBounds>(member.Entity);
        const auto* lod = world.GetComponent<Components::LODGroup>(member.Entity);
        Mathematics::Vector3 centerOffset = offset;
        float radius = 1.0f;
        if (bounds)
        {
            const auto matrix = transform->ToMatrix4x4(offset);
            centerOffset = matrix.TransformPoint(bounds->Box.center);
            radius = bounds->Box.Radius() * Rendering::MatrixUtils::LargestAxisScale(matrix.Data());
        }
        const float distance = radius * row.lodCoverageScale * std::exp2(lod ? lod->Bias : 0.0f) / threshold;
        const float centerDistance = centerOffset.Length();
        if (!(threshold > 0.0f) || !std::isfinite(distance) || !std::isfinite(centerDistance))
        {
            envelope.RequiredDistanceAtUnitProjection = std::numeric_limits<float>::infinity();
            return;
        }
        envelope.MaxCenterOffset = std::max(envelope.MaxCenterOffset, centerDistance);
        envelope.RequiredDistanceAtUnitProjection = std::max(envelope.RequiredDistanceAtUnitProjection, distance);
    }
}

bool MemberEnvelopeAllowsProxy(const Hlod::RuntimeCluster& cluster,
                               const Hlod::RuntimeCluster::LodEnvelope& envelope,
                               const Hlod::ViewLod& view)
{
    // The cluster-to-camera distance minus MaxCenterOffset bounds every member distance below.
    const float requiredDistance = envelope.MaxCenterOffset + view.ProjScaleY *
        std::exp2(view.LodBiasGlobal) * envelope.RequiredDistanceAtUnitProjection;
    return Hlod::ClusterDistance(view, cluster.SphereCenter) > requiredDistance;
}

Hlod::ViewLod MakeHlodView(const Rendering::CameraData& camera, float lodBias, bool wantsFullDetail)
{
    Hlod::ViewLod view;
    const bool orthographic = camera.cameraPos.w == 1.0f || std::abs(camera.proj[3][3]) > 0.5f;
    const auto eye = CameraEye(camera);
    const float projection = std::abs(camera.proj[1][1]);
    if (!wantsFullDetail && !orthographic && std::isfinite(projection)
        && std::isfinite(eye.X) && std::isfinite(eye.Y) && std::isfinite(eye.Z))
    {
        view.CameraPos[0] = eye.X;
        view.CameraPos[1] = eye.Y;
        view.CameraPos[2] = eye.Z;
        view.ProjScaleY = projection;
        view.LodBiasGlobal = lodBias;
    }
    return view;
}

} // namespace

struct HLODSelectSystem::ModelReloadInbox
{
    std::mutex Mutex;
    std::atomic<bool> Pending{false};
    std::vector<GUID> Models;
};

HLODSelectSystem::HLODSelectSystem(RenderServices* renderServices, AssetEventDispatcher* assetEvents)
    : m_RenderServices(renderServices)
{
    if (!m_RenderServices || !IsHlodRuntimeEnabled())
        return;
    if (!assetEvents)
    {
        if (auto* assets = EngineCore::GetInstance().TryGetAssetManager())
            assetEvents = &assets->GetEventDispatcher();
    }
    if (!assetEvents)
        return;

    m_ModelReloadInbox = std::make_shared<ModelReloadInbox>();
    m_ModelReloadSubscription = AssetReloadInvalidator(*assetEvents, AssetType::Model,
        [inbox = m_ModelReloadInbox](const GUID& model)
        {
            const std::lock_guard lock(inbox->Mutex);
            inbox->Models.push_back(model);
            inbox->Pending.store(true, std::memory_order_release);
        }, AssetReloadInvalidator::EventSet::ReloadedOnly);
}

void HLODSelectSystem::EnterProxy(ECS::World& world, Rendering::GPUScene& scene,
                                  Hlod::RuntimeCluster& cluster) {
    m_SlotScratch.clear();
    for (ECS::EntityHandle member : cluster.Members) {
        auto* gpu = world.GetComponentForWrite<Components::MeshGPUData>(member);
        if (!gpu)
            continue;
        if (gpu->instanceIndex != kUnassignedSlot)
            m_SlotScratch.push_back(gpu->instanceIndex);
        EvictOne(*gpu);
    }
    if (!m_SlotScratch.empty())
        scene.RemoveInstances(m_SlotScratch);

    // Un-evict the proxies; the forced full lane re-adds them (Op::Add on
    // instanceIndex == 0xFFFFFFFF).
    for (ECS::EntityHandle proxy : cluster.Proxies) {
        if (auto* gpu = world.GetComponentForWrite<Components::MeshGPUData>(proxy))
            gpu->hlodEvicted = false;
    }
    cluster.ProxyActive = true;
    Logger::Log::Debug("Hlod: selected proxy for {} source meshes", cluster.Members.size());
}

void HLODSelectSystem::ExitProxy(ECS::World& world, Rendering::GPUScene& scene,
                                 Hlod::RuntimeCluster& cluster) {
    m_SlotScratch.clear();
    for (ECS::EntityHandle proxy : cluster.Proxies) {
        auto* gpu = world.GetComponentForWrite<Components::MeshGPUData>(proxy);
        if (!gpu)
            continue;
        if (gpu->instanceIndex != kUnassignedSlot)
            m_SlotScratch.push_back(gpu->instanceIndex);
        EvictOne(*gpu);
    }
    if (!m_SlotScratch.empty())
        scene.RemoveInstances(m_SlotScratch);

    for (ECS::EntityHandle member : cluster.Members) {
        if (auto* gpu = world.GetComponentForWrite<Components::MeshGPUData>(member))
            gpu->hlodEvicted = false;
    }
    cluster.ProxyActive = false;
    Logger::Log::Debug("Hlod: restored {} source meshes", cluster.Members.size());
}

bool HLODSelectSystem::MembersChanged(const ECS::World& world, const Hlod::RuntimeCluster& cluster)
{
    for (size_t index = 0; index < cluster.Members.size(); ++index)
    {
        const ECS::EntityHandle member = cluster.Members[index];
        const auto* transform = world.GetComponent<Components::WorldTransform>(member);
        const auto* renderer = world.GetComponent<Components::MeshRenderer>(member);
        if (!transform || !renderer || transform->Version != cluster.MemberTransformVersion[index] ||
            RendererChanged(*renderer, cluster, index))
            return true;
    }
    return false;
}

void HLODSelectSystem::MarkEditedMembers(ECS::World& world, Hlod::HlodRuntime& runtime, uint64 entryVersion)
{
    auto transforms = world.Query<ECS::Read<Components::WorldTransform>>();
    transforms.With<Components::MeshRenderer>();
    transforms.IncludeDisabled();
    transforms.Changed<Components::WorldTransform>(m_TransformEditGate);
    transforms.Each([&runtime](ECS::EntityHandle entity, const Components::WorldTransform& transform)
    {
        for (const auto& location : runtime.FindMemberLocations(entity))
        {
            auto& cluster = runtime.GetClusters()[location.ClusterIndex];
            if (!cluster.Stale && transform.Version != cluster.MemberTransformVersion[location.MemberIndex])
                cluster.Stale = true;
        }
    });

    auto renderers = world.Query<ECS::Read<Components::MeshRenderer>>();
    renderers.With<Components::WorldTransform>();
    renderers.IncludeDisabled();
    renderers.Changed<Components::MeshRenderer>(m_MeshRendererEditGate);
    renderers.Each([&runtime](ECS::EntityHandle entity, const Components::MeshRenderer& renderer)
    {
        for (const auto& location : runtime.FindMemberLocations(entity))
        {
            auto& cluster = runtime.GetClusters()[location.ClusterIndex];
            if (!cluster.Stale && RendererChanged(renderer, cluster, location.MemberIndex))
                cluster.Stale = true;
        }
    });

    m_TransformEditGate.LastRunVersion = entryVersion;
    m_MeshRendererEditGate.LastRunVersion = entryVersion;
}

void HLODSelectSystem::Update(ECS::World& world, float32 /*deltaTime*/) {
    if (!IsHlodRuntimeEnabled() || !m_RenderServices)
        return;

    Hlod::HlodRuntime& runtime = m_RenderServices->GetHlodRuntime();
    const bool hasClusters = runtime.HasClusters();
    if (hasClusters && runtime.GetBoundWorldId() != world.GetWorldId())
        return;
    std::vector<GUID> reloadedModels;
    if (m_ModelReloadInbox && m_ModelReloadInbox->Pending.load(std::memory_order_acquire))
    {
        const std::lock_guard lock(m_ModelReloadInbox->Mutex);
        reloadedModels.swap(m_ModelReloadInbox->Models);
        m_ModelReloadInbox->Pending.store(false, std::memory_order_relaxed);
    }
    if (!hasClusters)
        return;
    std::sort(reloadedModels.begin(), reloadedModels.end());
    const auto& meshRegistry = m_RenderServices->GetMeshGPURegistry();
    if (!runtime.IsBoundTo(world)) {
        runtime.Retire(world, m_RenderServices->GetMeshGPURegistry());
        m_RenderServices->RequestResidencyExtraction();
        return;
    }

    Rendering::GPUScene* scene = m_RenderServices->GetGPUScene();
    if (!scene)
        return;

    // Residency is shared by all views. Keep the members while any view still needs them.
    m_DemandViews.clear();
    const auto& views = m_RenderServices->Views();
    const float lodBias = m_RenderServices->GetLODGlobalBias();
    const bool selectionOff = meshRegistry.GetLodSelectionMode() == Rendering::LodSelectionMode::Off;
    for (const auto& view : views.GetViews())
    {
        const auto layers = view.ActiveRenderLayerMask();
        if (layers == Rendering::RenderLayerMask::None() || (view.worldId != 0 && view.worldId != world.GetWorldId()))
            continue;
        if (!view.RenderedByPipeline() && !(view.purpose == Rendering::ViewPurpose::UtilityCapture && view.worldId != 0))
            continue;
        const uint32 forceLevel = m_RenderServices->ResolveViewForceLOD(view.id);
        const auto sse = m_RenderServices->ComputeViewSseScales(view.id);
        const auto* camera = views.FindCameraData(view.cameraId);
        m_DemandViews.push_back({camera ? MakeHlodView(*camera, lodBias, selectionOff || forceLevel == 0)
                                       : Hlod::ViewLod{},
                                 layers, forceLevel, sse.Default, sse.Tight});
    }
    // Hosts using a viewpoint without a view registry still get the single-camera policy.
    if (views.GetViews().empty())
    {
        if (const auto* camera = views.FindCameraData(views.GetViewpointCamera()))
        {
            const uint32 forceLevel = m_RenderServices->GetLODForceLevel();
            const auto sse = m_RenderServices->ComputeViewSseScales(Rendering::ViewId{});
            m_DemandViews.push_back({MakeHlodView(*camera, lodBias, selectionOff || forceLevel == 0),
                                     Rendering::RenderLayerMask::All(), forceLevel, sse.Default, sse.Tight});
        }
    }

    // Switch thresholds from the first enabled HLODVolume (v1: one applies
    // globally); default when none is present.
    Hlod::SwitchConfig switchConfig;
    {
        bool found = false;
        world.Query<ECS::Read<Components::HLODVolume>>().Each(
            [&](const Components::HLODVolume& volume) {
                if (found)
                    return;
                found = true;
                switchConfig = Hlod::MakeSwitchConfig(volume.SwitchCoverage, volume.SwitchHysteresis);
            });
    }

    // Changes to authored geometry or transforms stale the proxy. Bounds and
    // LOD bias edits only invalidate its cached eligibility.
    const std::size_t structuralVersion = world.GetStructuralChangeVersion();
    const uint64 reconcileRevision = runtime.GetReconcileRevision();
    const bool structureChanged = structuralVersion != m_StructuralVersion || reconcileRevision != m_ReconcileRevision;
    m_StructuralVersion = structuralVersion;
    m_ReconcileRevision = reconcileRevision;
    const uint64 probeVersion = world.GetGlobalSystemVersion();
    if (structureChanged)
    {
        // Destroying an entity can empty its chunk without stamping a column.
        m_TransformEditGate.LastRunVersion = probeVersion;
        m_MeshRendererEditGate.LastRunVersion = probeVersion;
    }
    else
    {
        MarkEditedMembers(world, runtime, probeVersion);
    }
    bool memberLodInputsChanged = structureChanged;
    {
        auto probe = [&](auto tag, ECS::ChangeGate& gate) {
            using C = decltype(tag);
            auto q = world.Query<ECS::Read<C>>();
            q.template Changed<C>(gate);
            bool fired = false;
            q.BatchEach([&](const C*, std::size_t) { fired = true; });
            gate.LastRunVersion = probeVersion;
            return fired;
        };
        memberLodInputsChanged |= probe(Components::LocalBounds{}, m_LocalBoundsEditGate);
        memberLodInputsChanged |= probe(Components::LODGroup{}, m_LodGroupEditGate);
    }
    if (memberLodInputsChanged)
        ++m_MemberLodInputRevision;

    const uint64 meshRevision = meshRegistry.GetContentRevision();
    const bool residencyChanged = meshRevision != m_ProxyMeshRevision;
    m_ProxyMeshRevision = meshRevision;
    bool anyFlip = false;
    for (Hlod::RuntimeCluster& cluster : runtime.GetClusters()) {
        if (structureChanged && !cluster.Stale)
            cluster.Stale = MembersChanged(world, cluster);
        if (cluster.Stale)
        {
            if (cluster.ProxyActive)
            {
                ExitProxy(world, *scene, cluster);
                anyFlip = true;
            }
            continue;
        }
        if (!reloadedModels.empty() && runtime.ReloadedGeometryChanged(world, cluster, reloadedModels))
        {
            cluster.Stale = true;
            if (cluster.ProxyActive)
            {
                ExitProxy(world, *scene, cluster);
                anyFlip = true;
            }
            continue;
        }

        float coverage = 0.0f;
        bool hasConsumer = false;
        for (const auto& view : m_DemandViews)
        {
            if (!view.Layers.Intersects(cluster.RenderLayers))
                continue;
            hasConsumer = true;
            if (view.ForceLevel < cluster.MaxBakedLevel)
            {
                coverage = Hlod::kAlwaysMembersCoverage;
                break;
            }
            coverage = std::max(coverage,
                Hlod::ClusterCoverage(view.Projection, cluster.SphereCenter, cluster.SphereRadius));
            if (coverage >= switchConfig.ExitCoverage)
                break;
        }
        bool wantProxy = hasConsumer ? Hlod::UpdateProxyActive(cluster.ProxyActive, coverage, switchConfig)
                                     : cluster.ProxyActive;
        if (wantProxy && hasConsumer && !cluster.CoarsenedMembers.empty())
        {
            size_t envelopeIndex = 0;
            for (const auto& view : m_DemandViews)
            {
                if (view.ForceLevel != kAutomaticLod || !view.Layers.Intersects(cluster.RenderLayers))
                    continue;
                if (envelopeIndex == cluster.MemberLodEnvelopes.size())
                    cluster.MemberLodEnvelopes.emplace_back();
                auto& envelope = cluster.MemberLodEnvelopes[envelopeIndex++];
                if (!envelope.Initialized || envelope.MeshRevision != meshRevision ||
                    envelope.InputRevision != m_MemberLodInputRevision ||
                    envelope.DefaultSseScale != view.DefaultSseScale || envelope.TightSseScale != view.TightSseScale)
                {
                    RebuildMemberEnvelope(world, meshRegistry, *scene, cluster, envelope, meshRevision,
                                          m_MemberLodInputRevision, view.DefaultSseScale, view.TightSseScale);
                }
                if (!MemberEnvelopeAllowsProxy(cluster, envelope, view.Projection))
                {
                    wantProxy = false;
                    break;
                }
            }
        }
        if (wantProxy && !cluster.ProxyActive)
            wantProxy = !HasSplitMember(world, cluster);
        if (wantProxy && (!cluster.ProxyActive || residencyChanged || structureChanged))
            wantProxy = AllProxiesDrawable(world, meshRegistry, cluster);
        if (wantProxy == cluster.ProxyActive)
            continue;

        if (wantProxy)
            EnterProxy(world, *scene, cluster);
        else
            ExitProxy(world, *scene, cluster);
        anyFlip = true;
    }

    // Any residency flip re-adds a set via the full lane (Op::Add).
    if (anyFlip)
        m_RenderServices->RequestResidencyExtraction();
}

} // namespace Engine::Renderer
} // namespace GameEngine
