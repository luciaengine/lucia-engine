#pragma once

#include "ECS/Entity.h"
#include "Types/FlatMap.h"
#include "Types/Types.h"

#include <memory>
#include <vector>

namespace JobSystem { class WorkStealingThreadPool; }

namespace GameEngine::ECS { class World; }

namespace GameEngine::TerrainECS
{

class AreaReferenceSurface;
struct ResolvedModifier;

// The reference surfaces of the flattens on closed spline areas, one per area entity, kept across
// gathers and rebuilt only when an area's inputs change: its rim, its footprint, or a road at one of
// its openings.
class AreaReferenceSurfaceCache
{
public:
    AreaReferenceSurfaceCache();
    ~AreaReferenceSurfaceCache();
    AreaReferenceSurfaceCache(const AreaReferenceSurfaceCache&) = delete;
    AreaReferenceSurfaceCache& operator=(const AreaReferenceSurfaceCache&) = delete;

    // Points ResolvedModifier::AreaSurface of each closed area whose flatten grades to the volume's
    // height. Builds the changed surfaces on `jobs` (on this thread when null) and drops the surfaces
    // of removed areas. Call once per gather, after every modifier's bounds and route exist.
    void Resolve(const ECS::World& world, std::vector<ResolvedModifier>& modifiers,
                 JobSystem::WorkStealingThreadPool* jobs);

    // The surfaces built since construction.
    uint32 GetBuildCountForTests() const { return m_BuildCount; }

private:
    FlatMap<ECS::EntityHandle, std::unique_ptr<AreaReferenceSurface>> m_Surfaces;
    uint32 m_BuildCount = 0;
};

} // namespace GameEngine::TerrainECS
