#pragma once

#include "AssetCore/GUID.h"
#include "Components/Rendering/MeshRenderer.h"
#include "ECS/Entity.h"
#include "Engine/Rendering/ClusterLOD/ClusterLODCut.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "Mathematics/Geometry.h"
#include "Types/Types.h"

#include <vector>

namespace GameEngine
{
namespace ECS { class World; }
namespace Rendering { class GPUScene; }
namespace Engine::Renderer
{

/// The instances of cluster models that draw as one part per DAG unit of their cut.
/// An instance draws its coarse mesh as one entity until a group under the resident level wants
/// to refine. Then the instance splits: runtime-only children draw the units of its cut, and its
/// own GPU instance is evicted. The owner and its parts never draw in the same frame.
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
        // Per group of the mesh: the decision of the cut that draws now.
        std::vector<ClusterLODCutDecision> Decisions;
        // Per unit of the mesh: the part, or a null handle, and the epoch it last drew.
        std::vector<ECS::EntityHandle> Parts;
        std::vector<uint64> PartEpochs;
        uint64 LastWantedEpoch = 0;
        // False until the parts of the first cut can draw; the owner draws until then.
        bool Active = false;
    };

    /// Starts an inactive split of `owner` with the cut `decisions` and no parts.
    Instance& Begin(ECS::EntityHandle owner, const Components::MeshRenderer& renderer, const GUID& model, uint32 mesh,
                    std::vector<ClusterLODCutDecision> decisions, uint32 unitCount, uint64 epoch);
    /// Creates the evicted part of `unit`, or points its existing part at `geometry`. Marks the
    /// part as used in `epoch`, so DestroyIdleParts keeps it.
    void SetPart(ECS::World& world, Instance& instance, uint32 unit, Rendering::MeshGPUHandle geometry,
                 const Mathematics::BoundingBox& bounds, uint64 epoch) const;
    /// True when the part of `unit` exists, has a resolved world transform and draws `geometry`.
    bool IsPartReady(const ECS::World& world, const Instance& instance, uint32 unit,
                     Rendering::MeshGPUHandle geometry) const;
    /// Draws or evicts the part of `unit`. Returns true when the part changed.
    bool ShowPart(ECS::World& world, Rendering::GPUScene& scene, Instance& instance, uint32 unit, bool drawn,
                  uint64 epoch) const;
    /// Swaps the owner for the parts of the first cut. Call it when every part is ready.
    void Activate(ECS::World& world, Rendering::GPUScene& scene, Instance& instance) const;
    /// Destroys the evicted parts that have not been used for `grace` frames.
    void DestroyIdleParts(ECS::World& world, Rendering::GPUScene& scene, Instance& instance, uint64 epoch,
                          uint64 grace) const;
    /// Marks the part of `unit` as used in `epoch`, so DestroyIdleParts keeps it.
    void KeepPart(Instance& instance, uint32 unit, uint64 epoch) const { instance.PartEpochs[unit] = epoch; }
    /// Restores the owner and destroys its parts.
    void End(ECS::World& world, Rendering::GPUScene* scene, ECS::EntityHandle owner);
    /// Forgets every instance and does not touch entities. Call it when the world changes generation.
    void Clear() { m_Instances.clear(); }

    Instance* Find(ECS::EntityHandle owner);
    std::vector<Instance>& Instances() { return m_Instances; }
    size_t Count() const { return m_Instances.size(); }

  private:
    std::vector<Instance> m_Instances;
};

} // namespace Engine::Renderer
} // namespace GameEngine
