#include "ClusterLODSplits.h"

#include "Components/GeneratedEntity.h"
#include "Components/Hierarchy.h"
#include "Components/Rendering/LocalBounds.h"
#include "Components/Rendering/MeshGPUData.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Transform.h"
#include "ECS/World.h"
#include "Rendering/Core/GPUScene.h"

#include <algorithm>

namespace GameEngine::Engine::Renderer
{
namespace
{
constexpr uint32 kUnassignedSlot = 0xFFFFFFFFu;

bool IsEvicted(const ECS::World& world, ECS::EntityHandle entity)
{
    const auto* gpu = world.GetComponent<Components::MeshGPUData>(entity);
    return !gpu || gpu->clusterEvicted;
}

// Frees the GPU instance of `entity` and keeps extraction from adding it again.
void Evict(ECS::World& world, Rendering::GPUScene& scene, ECS::EntityHandle entity)
{
    auto* gpu = world.GetComponentForWrite<Components::MeshGPUData>(entity);
    if (!gpu)
    {
        world.AddComponentImmediate(entity, Components::MeshGPUData{.clusterEvicted = true});
        return;
    }
    if (gpu->instanceIndex != kUnassignedSlot)
    {
        const uint32 slot = gpu->instanceIndex;
        scene.RemoveInstances(std::span<const uint32>(&slot, 1));
    }
    gpu->instanceIndex = kUnassignedSlot;
    gpu->meshIndex = kUnassignedSlot;
    gpu->materialIndex = kUnassignedSlot;
    gpu->clusterEvicted = true;
}

void Restore(ECS::World& world, ECS::EntityHandle entity)
{
    if (auto* gpu = world.GetComponentForWrite<Components::MeshGPUData>(entity))
        gpu->clusterEvicted = false;
}
} // namespace

ClusterLODSplits::Instance& ClusterLODSplits::Begin(ECS::EntityHandle owner, const Components::MeshRenderer& renderer,
                                                    const GUID& model, uint32 mesh,
                                                    std::vector<ClusterLODCutDecision> decisions, uint32 unitCount,
                                                    uint64 epoch)
{
    Instance instance;
    instance.Owner = owner;
    instance.Renderer = renderer;
    instance.Model = model;
    instance.Mesh = mesh;
    instance.Decisions = std::move(decisions);
    instance.Parts.assign(unitCount, ECS::EntityHandle{});
    instance.PartEpochs.assign(unitCount, epoch);
    instance.LastWantedEpoch = epoch;
    return m_Instances.emplace_back(std::move(instance));
}

void ClusterLODSplits::SetPart(ECS::World& world, Instance& instance, uint32 unit, Rendering::MeshGPUHandle geometry,
                               const Mathematics::BoundingBox& bounds, uint64 epoch) const
{
    instance.PartEpochs[unit] = epoch;
    const ECS::EntityHandle existing = instance.Parts[unit];
    if (world.IsValid(existing))
    {
        // A write grant marks the chunk's renderer column changed, and extraction then reads
        // every part in it again: take it only for a real change.
        const auto* current = world.GetComponent<Components::MeshRenderer>(existing);
        if (!current || current->meshGpuHandleId == geometry.id || !IsEvicted(world, existing))
            return;
        if (auto* renderer = world.GetComponentForWrite<Components::MeshRenderer>(existing))
            renderer->meshGpuHandleId = geometry.id;
        return;
    }
    // A part draws only its registry entry: it has no model, so extraction never binds it from one.
    Components::MeshRenderer partRenderer = instance.Renderer;
    partRenderer.modelAssetGuid = {};
    partRenderer.meshId = 0;
    partRenderer.MeshNameId = 0;
    partRenderer.meshGpuHandleId = geometry.id;
    const auto* ownerBounds = world.GetComponent<Components::LocalBounds>(instance.Owner);
    Components::LocalBounds partBounds{};
    partBounds.Box = bounds;
    partBounds.DynamicObject = ownerBounds ? ownerBounds->DynamicObject : true;
    partBounds.CastShadows = ownerBounds ? ownerBounds->CastShadows : true;
    ECS::Entity part = world.Create();
    part.Set(Components::Transform{});
    // A part sits at its owner's origin: it starts with the owner's world transform.
    if (const auto* ownerTransform = world.GetComponent<Components::WorldTransform>(instance.Owner))
        part.Set(*ownerTransform);
    part.Set(partRenderer);
    part.Set(partBounds);
    part.Set(Components::MeshGPUData{.clusterEvicted = true});
    part.Set(Components::GeneratedEntity{});
    part.Set(Components::Parent{instance.Owner});
    world.ProcessCommands();
    instance.Parts[unit] = part.GetHandle();
}

bool ClusterLODSplits::IsPartReady(const ECS::World& world, const Instance& instance, uint32 unit,
                                   Rendering::MeshGPUHandle geometry) const
{
    const ECS::EntityHandle part = instance.Parts[unit];
    if (!world.IsValid(part))
        return false;
    // A part without a resolved world transform would draw at the origin for one frame.
    const auto* transform = world.GetComponent<Components::WorldTransform>(part);
    const auto* renderer = world.GetComponent<Components::MeshRenderer>(part);
    return transform && transform->Version != 0u && renderer && renderer->meshGpuHandleId == geometry.id;
}

bool ClusterLODSplits::ShowPart(ECS::World& world, Rendering::GPUScene& scene, Instance& instance, uint32 unit,
                                bool drawn, uint64 epoch) const
{
    const ECS::EntityHandle part = instance.Parts[unit];
    if (drawn)
        instance.PartEpochs[unit] = epoch;
    if (!world.IsValid(part) || IsEvicted(world, part) != drawn)
        return false;
    if (drawn)
        Restore(world, part);
    else
        Evict(world, scene, part);
    return true;
}

void ClusterLODSplits::Activate(ECS::World& world, Rendering::GPUScene& scene, Instance& instance) const
{
    Evict(world, scene, instance.Owner);
    instance.Active = true;
}

void ClusterLODSplits::DestroyIdleParts(ECS::World& world, Rendering::GPUScene& scene, Instance& instance, uint64 epoch,
                                        uint64 grace) const
{
    bool destroyed = false;
    for (uint32 unit = 0; unit < instance.Parts.size(); ++unit)
    {
        const ECS::EntityHandle part = instance.Parts[unit];
        if (!world.IsValid(part) || !IsEvicted(world, part) || epoch - instance.PartEpochs[unit] < grace)
            continue;
        Evict(world, scene, part);
        world.DestroyEntity(part);
        instance.Parts[unit] = {};
        destroyed = true;
    }
    if (destroyed)
        world.ProcessCommands();
}

void ClusterLODSplits::End(ECS::World& world, Rendering::GPUScene* scene, ECS::EntityHandle owner)
{
    const auto found = std::find_if(m_Instances.begin(), m_Instances.end(),
                                    [owner](const Instance& instance) { return instance.Owner == owner; });
    if (found == m_Instances.end())
        return;
    if (world.IsValid(owner))
        Restore(world, owner);
    for (const ECS::EntityHandle part : found->Parts)
    {
        if (!world.IsValid(part))
            continue;
        if (scene)
            Evict(world, *scene, part);
        world.DestroyEntity(part);
    }
    m_Instances.erase(found);
    world.ProcessCommands();
}

ClusterLODSplits::Instance* ClusterLODSplits::Find(ECS::EntityHandle owner)
{
    const auto found = std::find_if(m_Instances.begin(), m_Instances.end(),
                                    [owner](const Instance& instance) { return instance.Owner == owner; });
    return found == m_Instances.end() ? nullptr : &*found;
}

} // namespace GameEngine::Engine::Renderer
