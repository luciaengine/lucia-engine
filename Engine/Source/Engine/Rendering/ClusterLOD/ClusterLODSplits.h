#pragma once

#include "AssetCore/GUID.h"
#include "Components/Rendering/MeshRenderer.h"
#include "ECS/Entity.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "Mathematics/Geometry.h"
#include "Types/Types.h"

#include <span>
#include <vector>

namespace GameEngine
{
namespace ECS { class World; }
namespace Rendering { class GPUScene; }
namespace Engine::Renderer
{

/// The instances of cluster models that draw as one part per cluster group.
/// An instance draws its coarse mesh as one entity until a group wants its fine page. Then the
/// instance splits: it gets one runtime-only child per group, and its own GPU instance is evicted.
/// The owner and its parts never draw in the same frame.
class ClusterLODSplits
{
  public:
    struct Instance
    {
        ECS::EntityHandle Owner;
        // The owner's renderer when the parts copied it; a change ends the split.
        Components::MeshRenderer Renderer;
        GUID Model;
        uint32 Mesh = 0;
        uint32 FirstPage = 0;
        std::vector<ECS::EntityHandle> Parts;
        uint64 LastWantedEpoch = 0;
        // False until every part has a resolved world transform; the owner draws until then.
        bool Active = false;
    };

    /// Creates evicted parts for `owner`, one per entry of `coarseParts`, in page order.
    void Begin(ECS::World& world, ECS::EntityHandle owner, const Components::MeshRenderer& renderer, const GUID& model,
               uint32 mesh, uint32 firstPage, std::span<const Rendering::MeshGPUHandle> coarseParts,
               std::span<const Mathematics::BoundingBox> bounds, uint64 epoch);
    /// Swaps the owner for its parts once the parts can draw. Returns true when the swap happened.
    bool TryActivate(ECS::World& world, Rendering::GPUScene& scene, Instance& instance) const;
    /// Restores the owner and destroys its parts.
    void End(ECS::World& world, Rendering::GPUScene* scene, ECS::EntityHandle owner);
    /// Forgets every instance without touching entities, for a world that no longer holds them.
    void Clear() { m_Instances.clear(); }

    Instance* Find(ECS::EntityHandle owner);
    std::vector<Instance>& Instances() { return m_Instances; }
    size_t Count() const { return m_Instances.size(); }

  private:
    std::vector<Instance> m_Instances;
};

} // namespace Engine::Renderer
} // namespace GameEngine
