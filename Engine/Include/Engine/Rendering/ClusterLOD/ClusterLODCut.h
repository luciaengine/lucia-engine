#pragma once

#include "Types/Types.h"

#include <span>

namespace GameEngine
{
struct MeshClusterLODManifest;
namespace Engine::Renderer
{

/// The frame inputs of one DAG group of one instance.
struct ClusterLODCutGroup
{
    // The projected error over the switch threshold: above 1 wants refinement, infinity must refine.
    double Demand = 0.0;
    bool WasRefined = false;
    // Every unit of the group can draw.
    bool UnitsReady = false;
    // Every unit the group produced can draw.
    bool ProducedReady = false;
};

enum class ClusterLODCutDecision : uint8
{
    Coarse,
    Refined,
    // Wants to refine and fits the budget, but its units cannot draw yet. Stays coarse.
    RefineBlocked,
    // Wants to coarsen, but the units it produced cannot draw yet. Stays refined.
    CoarsenBlocked,
};

/// One instance of one cluster mesh. `Groups` and `Decisions` hold one entry per group of the
/// mesh, in manifest order.
struct ClusterLODCutInstance
{
    const MeshClusterLODManifest* Manifest = nullptr;
    uint32 Mesh = 0;
    std::span<const ClusterLODCutGroup> Groups;
    std::span<ClusterLODCutDecision> Decisions;
};

/// Chooses the refined groups of every instance from the top of each DAG down. A group refines
/// only after every group that holds its produced units refined, so every cut is crack-free.
/// Groups refine in order of demand while the drawn triangles fit `triangleBudget` (0: no limit);
/// a group that must refine refines over the budget. A group changes state only when the units
/// of its new state can draw. Terminal groups are always refined. Returns the triangles of the
/// cuts plus `baseTriangles`.
uint64 SelectClusterLODCuts(std::span<const ClusterLODCutInstance> instances, uint64 triangleBudget,
                            uint64 baseTriangles);

/// True when the decision leaves the group refined in this frame.
inline bool IsClusterLODRefined(ClusterLODCutDecision decision)
{
    return decision == ClusterLODCutDecision::Refined || decision == ClusterLODCutDecision::CoarsenBlocked;
}

/// True when the unit draws in the cut: its group is refined and its producer is not.
/// `decisions` holds one entry per group of the unit's mesh.
bool IsClusterLODUnitDrawn(const MeshClusterLODManifest& manifest, uint32 unit,
                           std::span<const ClusterLODCutDecision> decisions);

} // namespace Engine::Renderer
} // namespace GameEngine
