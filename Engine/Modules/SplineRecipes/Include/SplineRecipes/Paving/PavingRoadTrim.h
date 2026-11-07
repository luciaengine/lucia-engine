#pragma once

#include "Mathematics/Matrix4x4.h"
#include "Mathematics/Vector3.h"
#include "SplineGeometry/SplineStation.h"
#include "Types/FlatMap.h"
#include "Types/Types.h"

#include <span>
#include <utility>
#include <vector>

namespace GameEngine::SplineRecipes
{

// Where a paved region takes over one arm of a road: the arc length the road's own strip skips and
// the edge it caps at. The paving builder derives it on every rebuild; nothing saves it.
struct PavingRoadTrim
{
    // The road's arc length at the node and at the approach stop, in world meters (PavedArmStop).
    // The strip skips the arc length between them, the shorter way round on a closed road.
    float32 NodeDistance = 0.0f;
    float32 StopDistance = 0.0f;
    // The road goes on past the stop: the strip caps square with the ends of its top on OpenLeft and
    // OpenRight, left and right of travel outward from the node, world positions at the region's heights.
    bool Open = false;
    Mathematics::Vector3 OpenLeft{};
    Mathematics::Vector3 OpenRight{};
    // The paving builds a curb up to the Open edge, so a verge that starts there meets it uncapped.
    bool Curbed = false;

    // Exact, member by member.
    bool operator==(const PavingRoadTrim& other) const;
};

// The trims of every road the paved regions take over. Key: the road's entity id.
using PavingRoadTrims = FlatMap<uint32, std::vector<PavingRoadTrim>>;

// World meters per local meter of a road's arc length under `worldMatrix`: the network's scale on
// the ground plane, which the trims' distances are measured in.
float32 RoadArcScale(const Mathematics::Matrix4x4& worldMatrix);

// The Open edge one end of a kept run lies on, in placer space.
struct TrimmedRunEdge
{
    bool Open = false;
    // PavingRoadTrim::Curbed.
    bool Curbed = false;
    // The ends of the strip's top on the edge, left and right of the run's travel.
    Mathematics::Vector3 Left{};
    Mathematics::Vector3 Right{};
};

// A swept road's stations less the arc length its trims skip, as runs back to back.
struct TrimmedSweepStations
{
    std::vector<SplineGeometry::SplineStripStation> Stations;
    // Per station, the input station it lies on or after: the drape sample it reads.
    std::vector<uint32> Source;
    // Per kept run, its first and last index into Stations.
    std::vector<std::pair<uint32, uint32>> Runs;
    // Per kept run, the Open edges at its first and at its last station.
    std::vector<std::pair<TrimmedRunEdge, TrimmedRunEdge>> RunEdges;
};

// Cuts `stations` (placer-local, at the ascending world arc lengths `roadDistance`) at `trims`. A run
// end at an Open stop lies on the Open edge, moved into placer space by `invPlacerWorld`, and its crown
// eases to flat over one road width before it (SplineStripStation::RiseScale). On a closed
// road (`roadLoopMeters` above 0), a run can cross the seam, and its distances keep rising.
TrimmedSweepStations TrimSweepStations(std::span<const SplineGeometry::SplineStripStation> stations,
                                       std::span<const float32> roadDistance, float32 roadLoopMeters,
                                       std::span<const PavingRoadTrim> trims,
                                       const Mathematics::Matrix4x4& invPlacerWorld);

} // namespace GameEngine::SplineRecipes
