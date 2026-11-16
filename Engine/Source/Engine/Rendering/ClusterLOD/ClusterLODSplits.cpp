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

void ClusterLODSplits::Begin(ECS::World& world, ECS::EntityHandle owner, const Components::MeshRenderer& renderer,
                             const GUID& model, uint32 mesh, uint32 firstPage, std::span<const Rendering::MeshGPUHandle> coarseParts,
                             std::span<const Mathematics::BoundingBox> bounds, uint64 epoch)
{
    Instance instance{owner, renderer, model, mesh, firstPage, {}, epoch, false};
    instance.Parts.reserve(coarseParts.size());
    const auto* ownerBounds = world.GetComponent<Components::LocalBounds>(owner);
    const auto* ownerTransform = world.GetComponent<Components::WorldTransform>(owner);
    for (size_t index = 0; index < coarseParts.size(); ++index)
    {
        // A part draws only its registry entry: it has no model, so extraction never binds it from one.
        Components::MeshRenderer partRenderer = renderer;
        partRenderer.modelAssetGuid = {};
        partRenderer.meshId = 0;
        partRenderer.MeshNameId = 0;
        partRenderer.meshGpuHandleId = coarseParts[index].id;
        Components::LocalBounds partBounds{};
        partBounds.Box = bounds[index];
        partBounds.DynamicObject = ownerBounds ? ownerBounds->DynamicObject : true;
        partBounds.CastShadows = ownerBounds ? ownerBounds->CastShadows : true;
        ECS::Entity part = world.Create();
        part.Set(Components::Transform{});
        // A part sits at its owner's origin: it starts with the owner's world transform.
        if (ownerTransform)
            part.Set(*ownerTransform);
        part.Set(partRenderer);
        part.Set(partBounds);
        part.Set(Components::MeshGPUData{.clusterEvicted = true});
        part.Set(Components::GeneratedEntity{});
        part.Set(Components::Parent{owner});
        instance.Parts.push_back(part.GetHandle());
    }
    world.ProcessCommands();
    m_Instances.push_back(std::move(instance));
}

bool ClusterLODSplits::TryActivate(ECS::World& world, Rendering::GPUScene& scene, Instance& instance) const
{
    if (instance.Active)
        return false;
    // A part without a resolved world transform would draw at the origin for one frame.
    for (const ECS::EntityHandle part : instance.Parts)
    {
        const auto* transform = world.GetComponent<Components::WorldTransform>(part);
        if (!transform || transform->Version == 0u)
            return false;
    }
    Evict(world, scene, instance.Owner);
    for (const ECS::EntityHandle part : instance.Parts)
        Restore(world, part);
    instance.Active = true;
    return true;
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
