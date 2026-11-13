#include "GridRegions.h"
#include "HexGridHelpers.h"
#include "Types/DisjointSets.h"

#include <cmath>

namespace GameEngine::Pathfinding
{

namespace
{

// The cells regions join over: the static blocked flags and heights, row-major, and the step limit.
struct StepCells
{
    std::span<const uint8> Blocked;
    std::span<const float32> Heights;
    float32 MaxStepHeight = 0.0f;

    bool IsOpen(uint32 cell) const { return Blocked[cell] == 0; }
};

// Joins the sets of the open `cell` and its neighbour `earlier` when the neighbour is open and an agent
// steps between them: AStarSolver's step-height test, the same expression, which reads the same both ways.
void JoinIfStep(const StepCells& cells, DisjointSets& sets, uint32 cell, uint32 earlier)
{
    if (!cells.IsOpen(earlier) || std::abs(cells.Heights[earlier] - cells.Heights[cell]) > cells.MaxStepHeight)
        return;
    sets.Join(cell, earlier);
}

// Row `z` of a square grid: each open cell joins its neighbours before it, (x - 1, z) and the three
// in row z - 1. A diagonal step needs both cardinal cells beside it open, as A*'s does.
void JoinSquareRow(const StepCells& cells, uint32 width, uint32 z, DisjointSets& sets)
{
    const uint32 rowStart = z * width;
    for (uint32 x = 0; x < width; ++x)
    {
        const uint32 cell = rowStart + x;
        if (!cells.IsOpen(cell))
            continue;
        if (x > 0)
            JoinIfStep(cells, sets, cell, cell - 1);
        if (z == 0)
            continue;
        const uint32 previousRow = cell - width;
        JoinIfStep(cells, sets, cell, previousRow);
        if (!cells.IsOpen(previousRow))
            continue;
        if (x > 0 && cells.IsOpen(cell - 1))
            JoinIfStep(cells, sets, cell, previousRow - 1);
        if (x + 1 < width && cells.IsOpen(cell + 1))
            JoinIfStep(cells, sets, cell, previousRow + 1);
    }
}

// Row `z` of a hex grid (odd-r): each open cell joins its neighbours before it, (x - 1, z) and the two
// in row z - 1 that HexGrid::GetNeighborOffsets gives for the row.
void JoinHexRow(const StepCells& cells, uint32 width, uint32 z, DisjointSets& sets)
{
    int32 offsetX[6] = {};
    int32 offsetZ[6] = {};
    uint32 offsetCount = 0;
    HexGrid::GetNeighborOffsets(z, offsetX, offsetZ, offsetCount);
    const uint32 rowStart = z * width;
    for (uint32 x = 0; x < width; ++x)
    {
        const uint32 cell = rowStart + x;
        if (!cells.IsOpen(cell))
            continue;
        if (x > 0)
            JoinIfStep(cells, sets, cell, cell - 1);
        if (z == 0)
            continue;
        for (uint32 offset = 0; offset < offsetCount; ++offset)
        {
            const int32 neighbourX = static_cast<int32>(x) + offsetX[offset];
            if (offsetZ[offset] != -1 || neighbourX < 0 || neighbourX >= static_cast<int32>(width))
                continue;
            JoinIfStep(cells, sets, cell, rowStart - width + static_cast<uint32>(neighbourX));
        }
    }
}

} // namespace

void GridRegions::Label(const GridSettings& settings, std::span<const uint8> blocked, std::span<const float32> heights,
                        DisjointSets& sets, std::vector<uint32>& outRegions)
{
    const uint32 width = settings.Width;
    const uint32 depth = settings.Depth;
    const size_t cellCount = static_cast<size_t>(width) * depth;
    sets.Reset(cellCount);
    outRegions.resize(cellCount);
    const StepCells cells{blocked, heights, settings.MaxStepHeight};
    for (uint32 z = 0; z < depth; ++z)
    {
        if (settings.Type == GridType::Hex)
            JoinHexRow(cells, width, z, sets);
        else
            JoinSquareRow(cells, width, z, sets);
    }
    // The root of a set is its smallest cell, so it is the region's id. A blocked cell joins no set.
    for (uint32 cell = 0; cell < cellCount; ++cell)
        outRegions[cell] = cells.IsOpen(cell) ? sets.Find(cell) : kNoRegion;
}

} // namespace GameEngine::Pathfinding
