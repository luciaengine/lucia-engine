#pragma once

#include "Pathfinding/PathfindingTypes.h"
#include "Types/Types.h"

#include <span>
#include <vector>

namespace GameEngine
{
class DisjointSets;
}

namespace GameEngine::Pathfinding
{

// The connected groups of a grid's cells that are not statically blocked. Two neighbours join when their heights differ
// by at most MaxStepHeight (square: eight, a diagonal only when both cardinal cells beside it are statically open; hex:
// six). Other rules only refuse steps, so a start and a goal in different regions have no path for any agent radius.
class GridRegions
{
public:
    // The region of a statically blocked cell: it is in none.
    static constexpr uint32 kNoRegion = UINT32_MAX;

    // Writes each cell's region to `outRegions`, resized to the cell count: the smallest cell index in the region, or
    // kNoRegion for a statically blocked cell. `blocked` and `heights` are the static flags and heights, row-major.
    // The cells join in `sets`, reset first; both keep their storage, so a relabel at the same size allocates nothing.
    static void Label(const GridSettings& settings, std::span<const uint8> blocked, std::span<const float32> heights,
                      DisjointSets& sets, std::vector<uint32>& outRegions);
};

} // namespace GameEngine::Pathfinding
