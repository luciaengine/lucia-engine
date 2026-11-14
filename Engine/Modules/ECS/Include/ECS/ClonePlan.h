#pragma once

#include "ECS/ECS.h"

#include <cstdint>
#include <vector>

namespace GameEngine::ECS
{

class Archetype;

/// Groups source entities and entity-field remaps for World::CreateBatchFromWorld.
/// Rebuild the plan after source archetypes or entity references change.
/// Repeated source occurrences form separate copies with matching internal references.
class ClonePlan
{
  private:
    friend class World;

    static constexpr std::uint32_t kNoPosition = UINT32_MAX;

    // The list positions of one source archetype. The group's targets are
    // EntityRemap's grouped targets [FirstTarget, FirstTarget + Count).
    struct Group
    {
        const Archetype* Source = nullptr; // null for an entity with no components
        std::uint32_t FirstTarget = 0;
        std::uint32_t Count = 0;
    };
    // One EntityHandle in the row at Position: in component TypeId at byte
    // Offset, naming the entity at TargetPosition, or kNoPosition for an
    // entity outside the list (the copy gets the invalid handle).
    struct FieldTarget
    {
        ComponentTypeId TypeId = 0;
        std::uint32_t Position = 0;
        std::uint32_t Offset = 0;
        std::uint32_t TargetPosition = kNoPosition;
    };

    std::uint64_t m_SourceWorldId = 0;
    std::vector<EntityHandle> m_Sources;
    // The sources in EntityRemap's grouped order: m_GroupedSources[i] is the source of grouped target i.
    std::vector<EntityHandle> m_GroupedSources;
    std::vector<std::uint32_t> m_GroupOfPosition;
    // Where each position's target lands in EntityRemap's grouped targets.
    std::vector<std::uint32_t> m_GroupedIndexOfPosition;
    std::vector<Group> m_Groups;
    std::vector<FieldTarget> m_FieldTargets; // in position order
};

} // namespace GameEngine::ECS
