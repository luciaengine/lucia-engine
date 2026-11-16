#include "Engine/Rendering/ClusterLOD/ClusterLODCut.h"

#include "Assets/ClusterLOD/MeshClusterLODAsset.h"

#include <cmath>
#include <limits>
#include <queue>
#include <vector>

namespace GameEngine::Engine::Renderer
{
namespace
{

struct Candidate
{
    double Priority;
    uint32 Instance;
    uint32 Group;

    bool operator<(const Candidate& other) const { return Priority < other.Priority; }
};

uint64 UnitTriangles(const MeshClusterLODManifest& manifest, uint32 first, uint32 count, const uint32* indirect)
{
    uint64 triangles = 0;
    for (uint32 index = 0; index < count; ++index)
        triangles += manifest.Units[indirect ? indirect[first + index] : first + index].IndexCount / 3;
    return triangles;
}

// A group that cannot coarsen, or must refine, takes infinite priority and ignores the budget.
double Priority(const ClusterLODCutGroup& group)
{
    if (std::isinf(group.Demand) || (group.WasRefined && !group.ProducedReady))
        return std::numeric_limits<double>::infinity();
    return group.Demand;
}

} // namespace

uint64 SelectClusterLODCuts(std::span<const ClusterLODCutInstance> instances, uint64 triangleBudget,
                            uint64 baseTriangles)
{
    uint64 triangles = baseTriangles;
    std::priority_queue<Candidate> candidates;
    // Per instance and group: the produced units whose group is not refined yet.
    std::vector<std::vector<uint32>> pending(instances.size());
    for (uint32 index = 0; index < instances.size(); ++index)
    {
        const auto& instance = instances[index];
        const auto& manifest = *instance.Manifest;
        const auto& range = manifest.Meshes[instance.Mesh];
        pending[index].resize(range.GroupCount);
        for (uint32 local = 0; local < range.GroupCount; ++local)
        {
            const auto& group = manifest.Groups[range.FirstGroup + local];
            instance.Decisions[local] = group.Terminal ? ClusterLODCutDecision::Refined : ClusterLODCutDecision::Coarse;
            pending[index][local] = group.ProducedCount;
            if (group.Terminal)
                triangles += UnitTriangles(manifest, group.FirstUnit, group.UnitCount, nullptr);
        }
        for (uint32 local = 0; local < range.GroupCount; ++local)
        {
            const auto& group = manifest.Groups[range.FirstGroup + local];
            if (!group.Terminal)
                continue;
            for (uint32 unit = group.FirstUnit; unit < group.FirstUnit + group.UnitCount; ++unit)
                if (const uint32 producer = manifest.Units[unit].Producer; producer != kMeshClusterLODSourceGroup)
                    --pending[index][producer - range.FirstGroup];
        }
        for (uint32 local = 0; local < range.GroupCount; ++local)
            if (!manifest.Groups[range.FirstGroup + local].Terminal && pending[index][local] == 0)
                candidates.push({Priority(instance.Groups[local]), index, local});
    }
    while (!candidates.empty())
    {
        const Candidate candidate = candidates.top();
        candidates.pop();
        const auto& instance = instances[candidate.Instance];
        const auto& manifest = *instance.Manifest;
        const auto& range = manifest.Meshes[instance.Mesh];
        const auto& state = instance.Groups[candidate.Group];
        const auto& group = manifest.Groups[range.FirstGroup + candidate.Group];
        const bool forced = std::isinf(candidate.Priority);
        if (!forced && state.Demand <= 1.0)
        {
            if (state.WasRefined)
                instance.Decisions[candidate.Group] = ClusterLODCutDecision::Coarse;
            continue;
        }
        const uint64 added = UnitTriangles(manifest, group.FirstUnit, group.UnitCount, nullptr);
        const uint64 removed = UnitTriangles(manifest, group.FirstProduced, group.ProducedCount,
                                             manifest.ProducedUnits.data());
        if (!forced && triangleBudget != 0 && triangles + added > triangleBudget + removed)
            continue;
        if (!state.WasRefined && !state.UnitsReady)
        {
            instance.Decisions[candidate.Group] = ClusterLODCutDecision::RefineBlocked;
            continue;
        }
        const bool coarsenBlocked = state.WasRefined && !state.ProducedReady && std::isfinite(state.Demand) &&
            state.Demand <= 1.0;
        instance.Decisions[candidate.Group] =
            coarsenBlocked ? ClusterLODCutDecision::CoarsenBlocked : ClusterLODCutDecision::Refined;
        triangles = triangles + added - removed;
        for (uint32 unit = group.FirstUnit; unit < group.FirstUnit + group.UnitCount; ++unit)
        {
            const uint32 producer = manifest.Units[unit].Producer;
            if (producer == kMeshClusterLODSourceGroup)
                continue;
            const uint32 local = producer - range.FirstGroup;
            if (--pending[candidate.Instance][local] == 0)
                candidates.push({Priority(instance.Groups[local]), candidate.Instance, local});
        }
    }
    return triangles;
}

bool IsClusterLODUnitDrawn(const MeshClusterLODManifest& manifest, uint32 unit,
                           std::span<const ClusterLODCutDecision> decisions)
{
    const auto& record = manifest.Units[unit];
    const uint32 firstGroup = manifest.Meshes[record.Mesh].FirstGroup;
    return IsClusterLODRefined(decisions[record.Group - firstGroup]) &&
        (record.Producer == kMeshClusterLODSourceGroup || !IsClusterLODRefined(decisions[record.Producer - firstGroup]));
}

} // namespace GameEngine::Engine::Renderer
