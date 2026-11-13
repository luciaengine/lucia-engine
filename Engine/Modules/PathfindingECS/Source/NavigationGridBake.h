#pragma once

#include "NavigationSourceCollector.h"

#include "Pathfinding/PathfindingTypes.h"
#include "Types/Types.h"

#include <memory>
#include <vector>

namespace GameEngine::Pathfinding
{
class GridMap;
} // namespace GameEngine::Pathfinding

namespace JobSystem
{
class WorkStealingThreadPool;
} // namespace JobSystem

namespace GameEngine::PathfindingECS
{

// What a grid bake reads, copied out of the world on the main thread so the bake itself reads
// nothing else and runs on any thread.
struct NavigationGridBakeInput
{
    Pathfinding::GridSettings Settings;
    // The static triangles; null when the bake builds them from Sources itself.
    std::shared_ptr<const NavigationStaticGeometry> Geometry;
    NavigationGeometrySources Sources;
    // The terrain's height under each cell centre, by cell index; the lowest float32 where the
    // centre lies outside the terrain's footprint.
    std::vector<float32> TerrainHeights;
    float32 StepHeight = 0.0f;
    float32 AgentHeight = 0.0f;
};

// Samples the terrain's finest height (RequireFinest) under each cell centre of `settings` into
// `outHeights`, by cell index. False when a centre's height is not on the CPU yet: `outWaitPoint` is
// the first such centre in cell order. Main thread: the terrain's height is main-thread data.
bool SampleNavigationGridTerrain(const Pathfinding::GridSettings& settings, const NavigationTerrainSource& terrain,
                                 std::vector<float32>& outHeights, Mathematics::Vector3& outWaitPoint);

// One grid bake in progress: the bake as one Background job on the pool, so no frame
// waits for it. The job owns its input and its result and touches nothing else: a bake destroyed
// with its job in flight cancels it: the job stops at the next chunk of its static triangles'
// build (when the job makes them) or of its grid passes, and the job's state is freed with the
// last reference.
class NavigationGridBake
{
  public:
    // Starts the bake on `pool`; with no pool, the constructor runs the whole bake on the caller.
    NavigationGridBake(NavigationGridBakeInput input, JobSystem::WorkStealingThreadPool* pool);
    ~NavigationGridBake();

    NavigationGridBake(const NavigationGridBake&) = delete;
    NavigationGridBake& operator=(const NavigationGridBake&) = delete;

    bool IsComplete() const;
    // The baked grid, once IsComplete. Rethrows what the bake threw, as a bake on the caller would.
    std::unique_ptr<Pathfinding::GridMap> TakeGrid();

  private:
    struct Job;
    // The job body: reads and writes only `job`.
    static void Run(Job& job, JobSystem::WorkStealingThreadPool* pool);

    std::shared_ptr<Job> m_Job;
};

} // namespace GameEngine::PathfindingECS
