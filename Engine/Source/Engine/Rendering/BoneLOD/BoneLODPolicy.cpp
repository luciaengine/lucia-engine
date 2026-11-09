#include "Engine/Rendering/BoneLOD/BoneLODPolicy.h"

#include "Animation/BoneEvaluationPlan.h"
#include "Assets/AssetManager.h"
#include "Assets/BoneLOD/ModelBoneLOD.h"
#include "Assets/ModelAsset.h"
#include "Components/Animation/AnimatedNodeRef.h"
#include "Components/Animation/Animator.h"
#include "Components/Animation/HumanoidRetargeterComponent.h"
#include "Components/Animation/SkeletonRef.h"
#include "Components/Rendering/LODGroup.h"
#include "Components/Rendering/LocalBounds.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Query.h"
#include "ECS/World.h"
#include "ECSModules/Rendering/SkeletonStore.h"
#include "Engine/Rendering/ViewRegistry.h"

#include <algorithm>
#include <cmath>

namespace GameEngine::Engine::Renderer
{

namespace
{
constexpr float kSwitchHysteresis = 0.25f;

bool RequiresFullPose(ECS::World& world, ECS::EntityHandle owner)
{
    const auto* animator = world.GetComponent<Components::Animator>(owner);
    return (animator && animator->source == Components::AnimatorPlaybackSource::Graph) ||
        world.HasComponent<Components::HumanoidRetargeterComponent>(owner);
}
} // namespace

void BoneLODPolicy::GatherViews(const ViewRegistry& views, uint64 worldId, float32 globalBias)
{
    for (const auto& view : views.GetViews())
    {
        const uint32 mask = view.ActiveRenderLayerMask();
        if (mask == 0u || (view.worldId != 0u && view.worldId != worldId))
            continue;
        FrameView entry;
        entry.LayerMask = mask;
        if (const auto* camera = views.FindCameraData(view.cameraId))
        {
            std::copy_n(camera->cameraPos, 3u, entry.View.CameraPos);
            entry.View.ProjScaleY = std::abs(camera->proj[5]);
            entry.View.LodBiasGlobal = globalBias;
            entry.Orthographic = camera->cameraPos[3] == 1.0f;
            entry.Valid = entry.View.ProjScaleY > 0.0f && std::isfinite(entry.View.ProjScaleY) &&
                std::isfinite(globalBias) && std::isfinite(camera->cameraPos[0]) &&
                std::isfinite(camera->cameraPos[1]) && std::isfinite(camera->cameraPos[2]) &&
                (camera->cameraPos[3] == 0.0f || camera->cameraPos[3] == 1.0f);
        }
        m_Views.push_back(entry);
    }
}

SharedPtr<const ModelBoneLOD> BoneLODPolicy::ResolveModel(const AssetManager& assets, const GUID& guid)
{
    if (const auto found = m_Models.find(guid); found != m_Models.end())
        return found->second;
    const auto asset = assets.GetAsset(guid);
    const auto* model = dynamic_cast<const ModelAsset*>(asset.get());
    auto data = model && model->IsLoaded() ? model->GetBoneLOD() : nullptr;
    m_Models.emplace(guid, data);
    return data;
}

void BoneLODPolicy::Accumulate(ECS::World& world, const AssetManager& assets, const Consumer& consumer)
{
    const auto& reference = consumer.Skeleton;
    if (reference.runtimeId == 0u)
        return;
    auto& store = SkeletonStore::Instance();
    const auto* runtime = store.GetRuntime(reference.runtimeId);
    const uint64 generation = store.GetRuntimeGeneration(reference.runtimeId);
    const auto* skeleton = store.Get(reference.skeletonId);
    if (!runtime || !skeleton || generation != reference.runtimeGeneration ||
        store.GetRuntimeSkeletonId(reference.runtimeId) != reference.skeletonId ||
        runtime->Source.ModelGuid.IsNull() || reference.sourceModelGuid.ToGuid() != runtime->Source.ModelGuid ||
        (consumer.Mesh && consumer.Mesh->modelAssetGuid.ToGuid() != runtime->Source.ModelGuid) ||
        runtime->Source.WorldId != world.GetWorldId() ||
        runtime->Source.WorldGeneration != world.GetLifecycleResetGeneration() ||
        !world.IsValid(runtime->Source.InstanceOwner) ||
        (reference.ownerMode == Components::SkeletonInstanceOwner::Entity &&
            reference.instanceOwner != runtime->Source.InstanceOwner))
    {
        m_ForceFull.insert(reference.runtimeId);
        return;
    }

    auto& state = m_Runtimes[reference.runtimeId];
    if (state.SeenEpoch != m_Epoch)
    {
        auto data = ResolveModel(assets, runtime->Source.ModelGuid);
        const bool unchanged = state.Selected.RuntimeGeneration == generation && state.Selected.Data == data;
        state.Selected.Reduced &= unchanged;
        state.Selected.Data = std::move(data);
        state.Selected.RuntimeGeneration = generation;
        state.SeenEpoch = m_Epoch;
        state.Demand = 0.0f;
        state.HasDemand = false;
        if (runtime->GraphPoseActive || RequiresFullPose(world, runtime->Source.InstanceOwner))
            m_ForceFull.insert(reference.runtimeId);
        const auto ownerSettings = m_Settings.find(runtime->Source.InstanceOwner);
        if (ownerSettings != m_Settings.end() && ownerSettings->second.FullDetailOverride)
            m_ForceFull.insert(reference.runtimeId);
    }
    auto settings = m_Settings.find(consumer.Entity);
    if (settings != m_Settings.end() && settings->second.FullDetailOverride)
        m_ForceFull.insert(reference.runtimeId);
    if (!consumer.Mesh && !consumer.Node && !consumer.Retargeter &&
        (!consumer.Animator || consumer.Animator->source != Components::AnimatorPlaybackSource::Graph))
        return;
    if (settings == m_Settings.end())
        settings = m_Settings.find(runtime->Source.InstanceOwner);
    const auto& data = state.Selected.Data;
    const auto* plan = data && data->ReducedPlan() ? data->ReducedPlan().get() : nullptr;
    if (settings == m_Settings.end() || settings->second.FullDetailOverride ||
        !std::isfinite(settings->second.SwitchCoverage) || settings->second.SwitchCoverage <= 0.0f ||
        !plan || !plan->IsForSkeleton(*skeleton) || consumer.Retargeter ||
        (consumer.Animator && consumer.Animator->source == Components::AnimatorPlaybackSource::Graph) ||
        (consumer.Node && (consumer.Node->nodeIndex >= skeleton->BoneCount ||
            plan->EvaluatedAncestors()[consumer.Node->nodeIndex] != consumer.Node->nodeIndex)))
    {
        m_ForceFull.insert(reference.runtimeId);
        return;
    }
    if (!consumer.Mesh)
        return;
    if (!consumer.Bounds || !consumer.Transform || (consumer.LOD && !std::isfinite(consumer.LOD->Bias)))
    {
        m_ForceFull.insert(reference.runtimeId);
        return;
    }

    const auto box = consumer.Bounds->Box.TransformToAABB(consumer.Transform->matrix);
    if (box.IsEmpty())
    {
        m_ForceFull.insert(reference.runtimeId);
        return;
    }
    const auto sphere = Mathematics::BoundingBox::FromMinMax(box.min, box.max);
    const float radius = sphere.Radius();
    if (!std::isfinite(radius) || radius <= 0.0f || !std::isfinite(sphere.center.x) ||
        !std::isfinite(sphere.center.y) || !std::isfinite(sphere.center.z))
    {
        m_ForceFull.insert(reference.runtimeId);
        return;
    }
    for (const auto& view : m_Views)
    {
        if ((view.LayerMask & consumer.Mesh->renderLayerMask) == 0u)
            continue;
        if (!view.Valid)
        {
            m_ForceFull.insert(reference.runtimeId);
            continue;
        }
        auto projection = view.View;
        if (consumer.LOD)
            projection.LodBiasGlobal += consumer.LOD->Bias;
        const float coverage = view.Orthographic
            ? radius * projection.ProjScaleY * std::exp2(projection.LodBiasGlobal)
            : Hlod::ClusterCoverage(projection, sphere.center.v, radius);
        const float demand = coverage / settings->second.SwitchCoverage;
        if (!std::isfinite(demand))
            m_ForceFull.insert(reference.runtimeId);
        else
        {
            state.Demand = std::max(state.Demand, demand);
            state.HasDemand = true;
        }
    }
}

void BoneLODPolicy::Update(ECS::World& world, const ViewRegistry* views, const AssetManager* assets, float32 globalBias)
{
    ++m_Epoch;
    m_Settings.clear();
    m_ForceFull.clear();
    m_Models.clear();
    m_Views.clear();
    if (m_WorldId != world.GetWorldId() || m_WorldGeneration != world.GetLifecycleResetGeneration())
    {
        m_Runtimes.clear();
        m_WorldId = world.GetWorldId();
        m_WorldGeneration = world.GetLifecycleResetGeneration();
    }
    world.Query<ECS::Read<Components::BoneLOD>>().Each(
        [this](ECS::EntityHandle entity, const Components::BoneLOD& settings) { m_Settings.emplace(entity, settings); });
    if (m_Settings.empty() || !views || !assets)
    {
        m_Runtimes.clear();
        return;
    }
    GatherViews(*views, world.GetWorldId(), globalBias);
    world.Query<ECS::Read<Components::SkeletonRef>, ECS::Optional<Components::LocalBounds>,
        ECS::Optional<Components::WorldTransform>, ECS::Optional<Components::MeshRenderer>,
        ECS::Optional<Components::AnimatedNodeRef>, ECS::Optional<Components::HumanoidRetargeterComponent>,
        ECS::Optional<Components::Animator>, ECS::Optional<Components::LODGroup>>().Each(
        [this, &world, assets](ECS::EntityHandle entity, const Components::SkeletonRef& reference,
            const Components::LocalBounds* bounds, const Components::WorldTransform* transform,
            const Components::MeshRenderer* mesh, const Components::AnimatedNodeRef* node,
            const Components::HumanoidRetargeterComponent* retargeter, const Components::Animator* animator,
            const Components::LODGroup* lod)
        { Accumulate(world, *assets, {entity, reference, bounds, transform, mesh, node, retargeter, animator, lod}); });

    const auto band = Hlod::MakeSwitchConfig(1.0f, kSwitchHysteresis);
    std::vector<uint32> retired;
    for (auto& [runtimeId, state] : m_Runtimes)
    {
        if (state.SeenEpoch != m_Epoch)
            retired.push_back(runtimeId);
        else
            state.Selected.Reduced = state.HasDemand && !m_ForceFull.contains(runtimeId) &&
                Hlod::UpdateProxyActive(state.Selected.Reduced, state.Demand, band);
    }
    for (uint32 runtimeId : retired)
        m_Runtimes.erase(runtimeId);
}

const BoneLODPolicy::Selection* BoneLODPolicy::Find(uint32 runtimeId) const
{
    const auto found = m_Runtimes.find(runtimeId);
    return found != m_Runtimes.end() ? &found->second.Selected : nullptr;
}

} // namespace GameEngine::Engine::Renderer
