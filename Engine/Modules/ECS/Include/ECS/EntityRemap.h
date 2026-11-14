#pragma once

#include "ECS/ECS.h"

#include <span>
#include <vector>

namespace GameEngine::ECS
{

class Archetype;

/// Result and reusable scratch storage for World::CreateBatchFromWorld.
/// GetTargets()[i] corresponds to entry i in the clone plan. Each call overwrites the result.
class EntityRemap
{
  public:
    /// The created entities, one per entry of the last call's plan, in the plan's order.
    std::span<const EntityHandle> GetTargets() const { return m_Targets; }

  private:
    friend class World;

    std::vector<EntityHandle> m_Targets;
    // The created entities grouped by the plan's archetype groups, for one Added batch per group.
    std::vector<EntityHandle> m_GroupedTargets;
    // The target archetype of each plan group.
    std::vector<Archetype*> m_TargetArchetypes;
};

} // namespace GameEngine::ECS
