#include "NavigationGridBake.h"

#include "JobSystem/ParallelAlgorithms.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Mathematics/Vector3.h"
#include "Pathfinding/GridMap.h"
#include "Pathfinding/PathfindingTypes.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <exception>
#include <limits>
#include <span>
#include <vector>

namespace GameEngine::PathfindingECS
{

namespace
{

constexpr float32 kNoGround = std::numeric_limits<float32>::lowest();
constexpr float32 kDegreesToRadians = 3.14159265358979323846f / 180.0f;
// Triangles one chunk of the triangle passes takes at least, so a small bake runs on the caller.
constexpr size_t kMinTrianglesPerChunk = 4096;
// Rows one chunk of the per-cell passes takes at least.
constexpr uint32 kMinRowsPerChunk = 8;
// How far outside a triangle's XZ bounds a cell centre is still tried against it, in metres and as
// a fraction of the bounds' larger side: HeightOnTriangle accepts a point a little outside the
// triangle (its kEdge tolerance and float32 rounding), so the cells tried cover every centre it
// can accept.
constexpr float32 kCandidateMargin = 1.0e-3f;
constexpr float32 kCandidateMarginFraction = 1.0e-4f;

Mathematics::Vector3 Vertex(const NavigationTriangles& triangles, uint32 triangle, uint32 corner)
{
    const float32* vertex = &triangles.Vertices[static_cast<size_t>(triangles.Indices[triangle * 3 + corner]) * 3];
    return {vertex[0], vertex[1], vertex[2]};
}

// The triangle's height above (x, z) when the point lies inside its XZ projection.
bool HeightOnTriangle(const Mathematics::Vector3& a, const Mathematics::Vector3& b, const Mathematics::Vector3& c,
                      float32 x, float32 z, float32& outHeight)
{
    const float32 det = (b.z - c.z) * (a.x - c.x) + (c.x - b.x) * (a.z - c.z);
    if (std::abs(det) < 1.0e-12f)
        return false;
    const float32 wa = ((b.z - c.z) * (x - c.x) + (c.x - b.x) * (z - c.z)) / det;
    const float32 wb = ((c.z - a.z) * (x - c.x) + (a.x - c.x) * (z - c.z)) / det;
    const float32 wc = 1.0f - wa - wb;
    constexpr float32 kEdge = -1.0e-5f;
    if (wa < kEdge || wb < kEdge || wc < kEdge)
        return false;
    outHeight = wa * a.y + wb * b.y + wc * c.y;
    return true;
}

// The rows (or columns) of `count`, `spacing` apart from `origin`, that [low, high] reaches, and
// `extraBelow` more below: false when it reaches none.
bool CellSpan(float32 low, float32 high, float32 origin, float32 spacing, uint32 count, uint32 extraBelow,
              uint32& outFirst, uint32& outLast)
{
    const float32 first = std::floor((low - origin) / spacing) - static_cast<float32>(extraBelow);
    const float32 last = std::floor((high - origin) / spacing);
    if (last < 0.0f || first >= static_cast<float32>(count))
        return false;
    outFirst = static_cast<uint32>(std::max(first, 0.0f));
    outLast = static_cast<uint32>(std::min(last, static_cast<float32>(count - 1)));
    return true;
}

// Where the grid's cells lie: a cell is the rectangle WorldToCell maps to it, `RowSpacing` deep and
// CellSize wide, the hex grid's odd rows offset by half a cell.
struct CellLattice
{
    const Pathfinding::GridMap& Grid;
    float32 FirstZ = 0.0f;
    float32 RowSpacing = 0.0f;

    explicit CellLattice(const Pathfinding::GridMap& grid)
        : Grid(grid)
    {
        float32 firstX = 0.0f, nextRowX = 0.0f, nextRowZ = 0.0f;
        grid.CellToWorld(0, 0, firstX, FirstZ);
        grid.CellToWorld(0, 1, nextRowX, nextRowZ);
        RowSpacing = nextRowZ - FirstZ;
    }

    // The cells whose rectangles the XZ box reaches: false when it reaches none. One column more
    // below covers the hex grid's odd rows.
    bool Span(float32 minX, float32 minZ, float32 maxX, float32 maxZ, uint32& outFirstRow, uint32& outLastRow,
              uint32& outFirstColumn, uint32& outLastColumn) const
    {
        const auto& settings = Grid.GetSettings();
        return CellSpan(minZ, maxZ, FirstZ - 0.5f * RowSpacing, RowSpacing, settings.Depth, 0, outFirstRow, outLastRow) &&
               CellSpan(minX, maxX, settings.OriginX, settings.CellSize, settings.Width, 1, outFirstColumn, outLastColumn);
    }
};

// One buffer of cells per chunk of a triangle pass, merged once the pass is done. The chunked
// JobSystem::ParallelFor runs at most kStackBatch chunks and does not tell a chunk its index, so each
// chunk claims the next buffer.
template <typename Cell>
class ChunkBuffers
{
  public:
    ChunkBuffers()
        : m_Buffers(JobSystem::WorkStealingThreadPool::kStackBatch)
    {
    }

    std::vector<Cell>& Claim()
    {
        const size_t slot = m_Claimed.fetch_add(1, std::memory_order_relaxed);
        assert(slot < m_Buffers.size());
        return m_Buffers[slot];
    }

    // The buffers claimed, once the pass has returned.
    std::span<std::vector<Cell>> Claimed() { return {m_Buffers.data(), m_Claimed.load(std::memory_order_relaxed)}; }

  private:
    std::vector<std::vector<Cell>> m_Buffers;
    std::atomic<size_t> m_Claimed{0};
};

// Raises `ground` (by cell index) to every up-facing triangle in [begin, end) over each cell
// centre it covers. A triangle tries only the centres inside its XZ bounds.
void RasterizeGround(const CellLattice& lattice, const NavigationTriangles& triangles, size_t begin, size_t end,
                     std::vector<float32>& ground)
{
    const Pathfinding::GridMap& grid = lattice.Grid;
    ground.assign(grid.GetCellCount(), kNoGround);
    for (size_t t = begin; t < end; ++t)
    {
        const uint32 triangle = static_cast<uint32>(t);
        const Mathematics::Vector3 a = Vertex(triangles, triangle, 0);
        const Mathematics::Vector3 b = Vertex(triangles, triangle, 1);
        const Mathematics::Vector3 c = Vertex(triangles, triangle, 2);
        // Recast's convention: the cross of (b - a) and (c - a) points up for a walkable face.
        const float32 upward = (b.z - a.z) * (c.x - a.x) - (b.x - a.x) * (c.z - a.z);
        if (!(upward > 0.0f))
            continue;
        const float32 minX = std::min({a.x, b.x, c.x}), maxX = std::max({a.x, b.x, c.x});
        const float32 minZ = std::min({a.z, b.z, c.z}), maxZ = std::max({a.z, b.z, c.z});
        const float32 margin = kCandidateMargin + kCandidateMarginFraction * std::max(maxX - minX, maxZ - minZ);
        uint32 firstRow = 0, lastRow = 0, firstColumn = 0, lastColumn = 0;
        if (!lattice.Span(minX - margin, minZ - margin, maxX + margin, maxZ + margin, firstRow, lastRow,
                          firstColumn, lastColumn))
            continue;
        for (uint32 cz = firstRow; cz <= lastRow; ++cz)
        {
            for (uint32 cx = firstColumn; cx <= lastColumn; ++cx)
            {
                float32 x = 0.0f, z = 0.0f;
                grid.CellToWorld(cx, cz, x, z);
                float32 height = 0.0f;
                if (HeightOnTriangle(a, b, c, x, z, height))
                {
                    float32& cell = ground[grid.CellIndex(cx, cz)];
                    cell = std::max(cell, height);
                }
            }
        }
    }
}

// Raises the cells [begin, end) of `ground` to the heights the other chunks' buffers hold.
void MergeHighest(std::span<const std::vector<float32>> others, size_t begin, size_t end, std::vector<float32>& ground)
{
    for (const std::vector<float32>& other : others)
        for (size_t i = begin; i < end; ++i)
            ground[i] = std::max(ground[i], other[i]);
}

// Writes to `ground` the highest up-facing static triangle under each cell centre, by cell index, or kNoGround.
// Every chunk of triangles keeps its own heights and the chunks are merged by maximum, so the
// result does not depend on the order. False when a pass stops on `cancelled`.
bool TriangleGround(const Pathfinding::GridMap& grid, const NavigationTriangles& triangles,
                    JobSystem::WorkStealingThreadPool* pool, const std::atomic<bool>& cancelled,
                    std::vector<float32>& ground)
{
    const CellLattice lattice(grid);
    const size_t cellCount = grid.GetCellCount();
    ChunkBuffers<float32> chunks;
    const bool rasterized = JobSystem::ParallelFor(
        pool, triangles.TriangleCount(),
        [&lattice, &triangles, &chunks, &cancelled](size_t begin, size_t end) {
            if (cancelled.load(std::memory_order_relaxed))
                return false;
            RasterizeGround(lattice, triangles, begin, end, chunks.Claim());
            return true;
        },
        kMinTrianglesPerChunk);
    if (!rasterized)
        return false;

    const std::span<std::vector<float32>> claimed = chunks.Claimed();
    if (claimed.empty())
    {
        ground.assign(cellCount, kNoGround);
        return true;
    }
    ground = std::move(claimed[0]);
    if (claimed.size() == 1)
        return true;
    return JobSystem::ParallelFor(
        pool, cellCount,
        [claimed, &ground, &cancelled](size_t begin, size_t end) {
            if (cancelled.load(std::memory_order_relaxed))
                return false;
            MergeHighest(claimed.subspan(1), begin, end, ground);
            return true;
        },
        size_t{kMinRowsPerChunk} * grid.GetSettings().Width);
}

// A static triangle clipped to a cell's XZ rectangle, with its heights: the triangle's three
// corners gain at most one per rectangle side.
struct ClippedTriangle
{
    Mathematics::Vector3 Points[7];
    uint32 Count = 0;
};

// Keeps the part of `polygon` on the inner side of the line `axis` (0 for X, 2 for Z) = `bound`:
// where (coordinate - bound) * `inward` is not negative.
void ClipToSide(ClippedTriangle& polygon, uint32 axis, float32 bound, float32 inward)
{
    ClippedTriangle kept;
    for (uint32 i = 0; i < polygon.Count; ++i)
    {
        const Mathematics::Vector3& a = polygon.Points[i];
        const Mathematics::Vector3& b = polygon.Points[(i + 1) % polygon.Count];
        const float32 insideA = (a[axis] - bound) * inward;
        const float32 insideB = (b[axis] - bound) * inward;
        if (insideA >= 0.0f)
            kept.Points[kept.Count++] = a;
        if ((insideA >= 0.0f) != (insideB >= 0.0f))
        {
            const float32 t = insideA / (insideA - insideB);
            Mathematics::Vector3& point = kept.Points[kept.Count++];
            for (uint32 k = 0; k < 3; ++k)
                point[k] = a[k] + (b[k] - a[k]) * t;
        }
    }
    polygon = kept;
}

// What an agent on a cell's ground gets past: a rise up to the climb between neighbouring cells
// (WalkableClimb over the cell spacing, the rise the slope pass allows between neighbours) below
// its feet, and nothing between its feet and its head.
struct AgentClearance
{
    float32 Climb = 0.0f;
    float32 Height = 0.0f;
};

// True when the clipped part of a steep triangle stands in the way of an agent on the cell's ground:
// its top rises above the climb and its bottom is below the agent's head. One threshold wherever in
// the cell the part stands, so moving a wall by part of a cell does not change whether it blocks.
bool Obstructs(const ClippedTriangle& part, float32 ground, const AgentClearance& agent)
{
    float32 low = std::numeric_limits<float32>::max();
    float32 high = std::numeric_limits<float32>::lowest();
    for (uint32 i = 0; i < part.Count; ++i)
    {
        low = std::min(low, part.Points[i].y);
        high = std::max(high, part.Points[i].y);
    }
    return high - ground > agent.Climb && low - ground < agent.Height;
}

// The cells' clearance test: what an agent on each cell's ground gets past, and the rectangle a
// steep face has to reach into.
struct ObstructionTest
{
    const CellLattice& Lattice;
    const std::vector<float32>& Ground;
    const std::vector<uint8>& HasGround;
    AgentClearance Agent;
    // Half the cell rectangle. A face on a cell border, as a box's side on a grid line, belongs to the
    // cell the box is in: the rectangle is shrunk by 1e-4 of a cell, so a face within that of a grid
    // line, as a fence mesh with no thickness laid exactly on one, belongs to neither cell and blocks
    // nothing.
    float32 HalfX = 0.0f;
    float32 HalfZ = 0.0f;
    float32 MinUpward = 0.0f;
};

// Marks in `marked` (by cell index) every cell with ground that a triangle in [begin, end) steeper
// than MaxSlope stands in.
void MarkObstructedRange(const ObstructionTest& test, const NavigationTriangles& triangles, size_t begin, size_t end,
                         std::vector<uint8>& marked)
{
    const Pathfinding::GridMap& grid = test.Lattice.Grid;
    marked.assign(grid.GetCellCount(), 0);
    for (size_t t = begin; t < end; ++t)
    {
        ClippedTriangle triangle;
        triangle.Count = 3;
        for (uint32 corner = 0; corner < 3; ++corner)
            triangle.Points[corner] = Vertex(triangles, static_cast<uint32>(t), corner);
        const Mathematics::Vector3& a = triangle.Points[0];
        const Mathematics::Vector3& b = triangle.Points[1];
        const Mathematics::Vector3& c = triangle.Points[2];
        const float32 normalX = (b.y - a.y) * (c.z - a.z) - (b.z - a.z) * (c.y - a.y);
        const float32 normalY = (b.z - a.z) * (c.x - a.x) - (b.x - a.x) * (c.z - a.z);
        const float32 normalZ = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
        const float32 length = std::sqrt(normalX * normalX + normalY * normalY + normalZ * normalZ);
        // Ground within MaxSlope is read at the centres; a degenerate triangle has no face.
        if (!(length > 0.0f) || normalY >= test.MinUpward * length)
            continue;

        const float32 minX = std::min({a.x, b.x, c.x}), maxX = std::max({a.x, b.x, c.x});
        const float32 minZ = std::min({a.z, b.z, c.z}), maxZ = std::max({a.z, b.z, c.z});
        const float32 low = std::min({a.y, b.y, c.y}), high = std::max({a.y, b.y, c.y});
        uint32 firstRow = 0, lastRow = 0, firstColumn = 0, lastColumn = 0;
        if (!test.Lattice.Span(minX, minZ, maxX, maxZ, firstRow, lastRow, firstColumn, lastColumn))
            continue;

        for (uint32 cz = firstRow; cz <= lastRow; ++cz)
        {
            for (uint32 cx = firstColumn; cx <= lastColumn; ++cx)
            {
                const uint32 cell = grid.CellIndex(cx, cz);
                if (!test.HasGround[cell] || marked[cell])
                    continue;
                // The whole triangle under a step or over a head: no part of it is in the way. This
                // passes the floor under the cells, the commonest steep face, without clipping it.
                const float32 ground = test.Ground[cell];
                if (high - ground <= test.Agent.Climb || low - ground >= test.Agent.Height)
                    continue;
                float32 centreX = 0.0f, centreZ = 0.0f;
                grid.CellToWorld(cx, cz, centreX, centreZ);
                if (maxX <= centreX - test.HalfX || minX >= centreX + test.HalfX || maxZ <= centreZ - test.HalfZ ||
                    minZ >= centreZ + test.HalfZ)
                    continue;
                ClippedTriangle part = triangle;
                ClipToSide(part, 0, centreX - test.HalfX, 1.0f);
                ClipToSide(part, 0, centreX + test.HalfX, -1.0f);
                ClipToSide(part, 2, centreZ - test.HalfZ, 1.0f);
                ClipToSide(part, 2, centreZ + test.HalfZ, -1.0f);
                if (part.Count > 0 && Obstructs(part, ground, test.Agent))
                    marked[cell] = 1;
            }
        }
    }
}

// Marks the cells [begin, end) of `marked` that the other chunks' buffers mark.
void MergeMarked(std::span<const std::vector<uint8>> others, size_t begin, size_t end, std::vector<uint8>& marked)
{
    for (const std::vector<uint8>& other : others)
        for (size_t i = begin; i < end; ++i)
            marked[i] |= other[i];
}

// Marks in `obstructed` every cell with ground that a static triangle steeper than MaxSlope stands in, anywhere in
// the cell: ground is read at cell centres only. The test is exact for a box; a sphere or a capsule is the
// collector's low-poly hull. The result does not depend on chunk order. False when a pass stops on `cancelled`.
bool MarkObstructedCells(const Pathfinding::GridMap& grid, const NavigationTriangles& triangles,
                         const std::vector<float32>& ground, const std::vector<uint8>& hasGround,
                         const AgentClearance& agent, JobSystem::WorkStealingThreadPool* pool,
                         const std::atomic<bool>& cancelled, std::vector<uint8>& obstructed)
{
    const auto& settings = grid.GetSettings();
    const CellLattice lattice(grid);
    const ObstructionTest test{lattice,
                               ground,
                               hasGround,
                               agent,
                               0.5f * settings.CellSize * (1.0f - 1.0e-4f),
                               0.5f * lattice.RowSpacing * (1.0f - 1.0e-4f),
                               std::cos(settings.MaxSlope * kDegreesToRadians)};
    const size_t cellCount = grid.GetCellCount();
    ChunkBuffers<uint8> chunks;
    const bool marked = JobSystem::ParallelFor(
        pool, triangles.TriangleCount(),
        [&test, &triangles, &chunks, &cancelled](size_t begin, size_t end) {
            if (cancelled.load(std::memory_order_relaxed))
                return false;
            MarkObstructedRange(test, triangles, begin, end, chunks.Claim());
            return true;
        },
        kMinTrianglesPerChunk);
    if (!marked)
        return false;

    const std::span<std::vector<uint8>> claimed = chunks.Claimed();
    if (claimed.empty())
    {
        obstructed.assign(cellCount, 0);
        return true;
    }
    obstructed = std::move(claimed[0]);
    if (claimed.size() == 1)
        return true;
    return JobSystem::ParallelFor(
        pool, cellCount,
        [claimed, &obstructed, &cancelled](size_t begin, size_t end) {
            if (cancelled.load(std::memory_order_relaxed))
                return false;
            MergeMarked(claimed.subspan(1), begin, end, obstructed);
            return true;
        },
        size_t{kMinRowsPerChunk} * settings.Width);
}

// Marks in `steep` every cell with ground, not obstructed, in rows [firstRow, endRow) whose ground
// differs from a neighbour's by more than MaxSlope allows over the distance between their centres;
// per-neighbour distances keep diagonals and hex cells right. A neighbour with no ground has no
// height to compare. A cell reads its neighbours and writes only itself.
void MarkSteepRows(const Pathfinding::GridMap& grid, const std::vector<float32>& ground,
                   const std::vector<uint8>& hasGround, const std::vector<uint8>& obstructed, uint32 firstRow,
                   uint32 endRow, std::vector<uint8>& steep)
{
    const auto& settings = grid.GetSettings();
    const float32 maxSlopeTangent = std::tan(settings.MaxSlope * kDegreesToRadians);
    for (uint32 cz = firstRow; cz < endRow; ++cz)
    {
        for (uint32 cx = 0; cx < settings.Width; ++cx)
        {
            const uint32 cell = grid.CellIndex(cx, cz);
            if (!hasGround[cell] || obstructed[cell])
                continue;
            float32 cellX = 0.0f, cellZ = 0.0f;
            grid.CellToWorld(cx, cz, cellX, cellZ);

            uint32 neighbourX[8], neighbourZ[8];
            const uint32 neighbours = grid.GetNeighborCells(cx, cz, neighbourX, neighbourZ, 8);
            for (uint32 i = 0; i < neighbours; ++i)
            {
                const uint32 neighbour = grid.CellIndex(neighbourX[i], neighbourZ[i]);
                if (!hasGround[neighbour])
                    continue;
                float32 x = 0.0f, z = 0.0f;
                grid.CellToWorld(neighbourX[i], neighbourZ[i], x, z);
                const float32 distance = std::sqrt((x - cellX) * (x - cellX) + (z - cellZ) * (z - cellZ));
                if (std::abs(ground[neighbour] - ground[cell]) > maxSlopeTangent * distance)
                {
                    steep[cell] = 1;
                    break;
                }
            }
        }
    }
}

// Bakes the grid `input` describes. A cell's ground is the highest of the terrain and the up-facing static
// triangles under its centre; a cell is blocked with no ground, or when it is steep or obstructed. Reads only
// `input`; a null `pool` runs it on the caller. Null when the bake reads `cancelled` set before it writes the cells.
std::unique_ptr<Pathfinding::GridMap> BakeNavigationGrid(NavigationGridBakeInput input,
                                                         JobSystem::WorkStealingThreadPool* pool,
                                                         const std::atomic<bool>& cancelled)
{
    if (cancelled.load(std::memory_order_relaxed))
        return nullptr;
    if (!input.Geometry)
        input.Geometry = std::make_shared<const NavigationStaticGeometry>(
            BuildNavigationStaticGeometry(std::move(input.Sources), pool, &cancelled));
    if (cancelled.load(std::memory_order_relaxed))
        return nullptr;
    auto grid = std::make_unique<Pathfinding::GridMap>(input.Settings);
    const NavigationTriangles& triangles = input.Geometry->Triangles;
    const size_t cellCount = grid->GetCellCount();

    std::vector<float32> ground;
    if (!TriangleGround(*grid, triangles, pool, cancelled, ground))
        return nullptr;
    std::vector<uint8> hasGround(cellCount, 0);
    for (size_t i = 0; i < cellCount; ++i)
    {
        ground[i] = std::max(ground[i], input.TerrainHeights[i]);
        hasGround[i] = ground[i] != kNoGround;
    }

    const float32 climb = Pathfinding::WalkableClimb(input.StepHeight, input.Settings.MaxSlope, input.Settings.CellSize);
    std::vector<uint8> obstructed, steep(cellCount, 0);
    if (!MarkObstructedCells(*grid, triangles, ground, hasGround, {climb, input.AgentHeight}, pool, cancelled,
                             obstructed))
        return nullptr;
    const bool steepMarked = JobSystem::ParallelFor(
        pool, grid->GetSettings().Depth,
        [&grid = *grid, &ground, &hasGround, &obstructed, &steep, &cancelled](size_t begin, size_t end) {
            if (cancelled.load(std::memory_order_relaxed))
                return false;
            MarkSteepRows(grid, ground, hasGround, obstructed, static_cast<uint32>(begin), static_cast<uint32>(end),
                          steep);
            return true;
        },
        kMinRowsPerChunk);
    if (!steepMarked)
        return nullptr;

    // One write per cell, then one per blocked cell with ground: the writes, and so the grid's
    // topology version, are those of a cell-by-cell bake.
    for (uint32 i = 0; i < cellCount; ++i)
    {
        uint32 cx = 0, cz = 0;
        grid->CellFromIndex(i, cx, cz);
        if (!hasGround[i])
            grid->SetCellBlocked(cx, cz, true);
        else
            grid->SetCellHeight(cx, cz, ground[i]);
    }
    for (uint32 i = 0; i < cellCount; ++i)
    {
        if (!hasGround[i] || !(obstructed[i] || steep[i]))
            continue;
        uint32 cx = 0, cz = 0;
        grid->CellFromIndex(i, cx, cz);
        grid->SetCellBlocked(cx, cz, true);
    }
    return grid;
}

} // namespace

bool SampleNavigationGridTerrain(const Pathfinding::GridSettings& settings, const NavigationTerrainSource& terrain,
                                 std::vector<float32>& outHeights, Mathematics::Vector3& outWaitPoint)
{
    const Pathfinding::GridMap lattice(settings);
    outHeights.assign(lattice.GetCellCount(), kNoGround);
    for (uint32 cz = 0; cz < settings.Depth; ++cz)
    {
        for (uint32 cx = 0; cx < settings.Width; ++cx)
        {
            float32 x = 0.0f, z = 0.0f;
            lattice.CellToWorld(cx, cz, x, z);
            if (!terrain.Query.ContainsXZ(x, z))
                continue;
            TerrainECS::SurfaceHeightSample sample;
            if (!terrain.Query.SampleHeight(Mathematics::Vector3(x, 0.0f, z),
                                            TerrainECS::SurfaceHeightPolicy::RequireFinest, sample))
            {
                outWaitPoint = Mathematics::Vector3(x, 0.0f, z);
                return false;
            }
            outHeights[lattice.CellIndex(cx, cz)] = sample.Height;
        }
    }
    return true;
}

struct NavigationGridBake::Job
{
    NavigationGridBakeInput Input;
    // Written by the job before Finished is released; read by the owner after acquiring it.
    std::unique_ptr<Pathfinding::GridMap> Grid;
    std::exception_ptr Error;
    std::atomic<bool> Finished{false};
    // Set only by the owner's destructor, so the owner never reads the null grid a cancelled bake
    // leaves.
    std::atomic<bool> Cancelled{false};
};

void NavigationGridBake::Run(Job& job, JobSystem::WorkStealingThreadPool* pool)
{
    try
    {
        job.Grid = BakeNavigationGrid(std::move(job.Input), pool, job.Cancelled);
    }
    catch (...)
    {
        job.Error = std::current_exception();
    }
    job.Finished.store(true, std::memory_order_release);
}

NavigationGridBake::NavigationGridBake(NavigationGridBakeInput input, JobSystem::WorkStealingThreadPool* pool)
    : m_Job(std::make_shared<Job>())
{
    m_Job->Input = std::move(input);
    if (!pool)
    {
        Run(*m_Job, nullptr);
        return;
    }
    pool->Submit([job = m_Job, pool]() { Run(*job, pool); }, JobSystem::JobPriority::Background);
}

NavigationGridBake::~NavigationGridBake()
{
    m_Job->Cancelled.store(true, std::memory_order_relaxed);
}

bool NavigationGridBake::IsComplete() const
{
    return m_Job->Finished.load(std::memory_order_acquire);
}

std::unique_ptr<Pathfinding::GridMap> NavigationGridBake::TakeGrid()
{
    if (m_Job->Error)
        std::rethrow_exception(m_Job->Error);
    return std::move(m_Job->Grid);
}

} // namespace GameEngine::PathfindingECS
