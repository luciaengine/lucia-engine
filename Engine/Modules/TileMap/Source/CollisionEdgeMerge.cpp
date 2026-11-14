#include "TileMap/CollisionEdgeMerge.h"

#include "Logger/Logger.h"

#include <algorithm>
#include <array>
#include <bitset>
#include <cmath>
#include <unordered_map>

namespace GameEngine
{
namespace TileMap
{
namespace
{

// Tolerance for classifying authored points as unit-square corners; collision
// authoring snaps far coarser than this.
constexpr float32 kCornerEpsilon = 1.0e-4f;

bool NearlyEqual(float32 a, float32 b)
{
    return std::fabs(a - b) <= kCornerEpsilon;
}

bool IsCorner(const Mathematics::Vector2& p, float32 x, float32 y)
{
    return NearlyEqual(p.x, x) && NearlyEqual(p.y, y);
}

// One occupancy grid per distinct (mode, material) pair in the chunk. Rectangles
// may only merge within a group: a merged rectangle carries a single mode and
// material, so merging across a boundary would silently relabel one side's
// tiles — an ice tile beside a normal one would take whichever won.
struct MergeGroup
{
    TileCollisionMode Mode = TileCollisionMode::Solid;
    GUID Material;
    std::bitset<kChunkCellCount> Occupancy;
};

/**
 * Greedy row-scan rectangle merge over one group's occupancy grid: each scan
 * (bottom row first) starts a rectangle at the first unconsumed occupied cell,
 * grows it right as far as the row allows, then grows it up while every row
 * above repeats the same span. Greedy is not globally optimal, but is linear in
 * cells and collapses the common solid-floor/platform layouts to one shape.
 */
void MergeOccupancyToRects(const MergeGroup& group, std::vector<CollisionRect>& outRects)
{
    std::bitset<kChunkCellCount> consumed;
    for (int32 y = 0; y < kChunkSize; ++y)
    {
        for (int32 x = 0; x < kChunkSize; ++x)
        {
            const size_t index = static_cast<size_t>(y * kChunkSize + x);
            if (!group.Occupancy.test(index) || consumed.test(index))
                continue;

            // Grow right along this row.
            int32 width = 1;
            while (x + width < kChunkSize)
            {
                const size_t next = static_cast<size_t>(y * kChunkSize + x + width);
                if (!group.Occupancy.test(next) || consumed.test(next))
                    break;
                ++width;
            }

            // Grow up while the full span repeats.
            int32 height = 1;
            while (y + height < kChunkSize)
            {
                bool rowFull = true;
                for (int32 dx = 0; dx < width; ++dx)
                {
                    const size_t above = static_cast<size_t>((y + height) * kChunkSize + x + dx);
                    if (!group.Occupancy.test(above) || consumed.test(above))
                    {
                        rowFull = false;
                        break;
                    }
                }
                if (!rowFull)
                    break;
                ++height;
            }

            for (int32 dy = 0; dy < height; ++dy)
                for (int32 dx = 0; dx < width; ++dx)
                    consumed.set(static_cast<size_t>((y + dy) * kChunkSize + x + dx));

            outRects.push_back(CollisionRect{static_cast<float32>(x), static_cast<float32>(y),
                                             static_cast<float32>(x + width),
                                             static_cast<float32>(y + height), group.Mode,
                                             group.Material});
        }
    }
}

// Below this, a corner's turn is treated as straight — it is an area in cell²
// units, so it sits far under any authored feature.
constexpr float32 kTurnEpsilon = 1.0e-6f;

using Ring = std::vector<Mathematics::Vector2>;

float32 SignedArea2(const Ring& ring)
{
    float32 sum = 0.0f;
    for (size_t i = 0; i < ring.size(); ++i)
    {
        const Mathematics::Vector2& a = ring[i];
        const Mathematics::Vector2& b = ring[(i + 1) % ring.size()];
        sum += a.x * b.y - b.x * a.y;
    }
    return sum;
}

float32 Turn(const Mathematics::Vector2& a, const Mathematics::Vector2& b,
             const Mathematics::Vector2& c)
{
    return (b.x - a.x) * (c.y - b.y) - (b.y - a.y) * (c.x - b.x);
}

bool IsConvexRing(const Ring& ring)
{
    bool left = false;
    bool right = false;
    for (size_t i = 0; i < ring.size(); ++i)
    {
        const float32 turn =
            Turn(ring[i], ring[(i + 1) % ring.size()], ring[(i + 2) % ring.size()]);
        left = left || turn > kTurnEpsilon;
        right = right || turn < -kTurnEpsilon;
    }
    return !(left && right);
}

bool TriangleContains(const Mathematics::Vector2& a, const Mathematics::Vector2& b,
                      const Mathematics::Vector2& c, const Mathematics::Vector2& p)
{
    // The ring is counter-clockwise by the time this runs, so an interior point
    // is left of all three edges.
    return Turn(a, b, p) >= 0.0f && Turn(b, c, p) >= 0.0f && Turn(c, a, p) >= 0.0f;
}

/**
 * Splits one authored ring into convex pieces by ear clipping. The chunk baker
 * extrudes each emitted piece through a convex hull, so a concave ring that
 * reached it unsplit would silently collide as its hull — an L or a notch would
 * come out filled in. A ring that is already convex passes through untouched,
 * point order included.
 */
void SplitToConvexPieces(Ring ring, uint32 tileId, std::vector<Ring>& outPieces)
{
    if (IsConvexRing(ring))
    {
        outPieces.push_back(std::move(ring));
        return;
    }

    if (SignedArea2(ring) < 0.0f)
        std::reverse(ring.begin(), ring.end());

    while (ring.size() > 3)
    {
        bool clipped = false;
        for (size_t i = 0; i < ring.size(); ++i)
        {
            const size_t prev = (i + ring.size() - 1) % ring.size();
            const size_t next = (i + 1) % ring.size();
            if (Turn(ring[prev], ring[i], ring[next]) <= kTurnEpsilon)
                continue; // reflex or straight — not an ear

            bool encloses = false;
            for (size_t j = 0; j < ring.size() && !encloses; ++j)
            {
                if (j == prev || j == i || j == next)
                    continue;
                encloses = TriangleContains(ring[prev], ring[i], ring[next], ring[j]);
            }
            if (encloses)
                continue;

            outPieces.push_back(Ring{ring[prev], ring[i], ring[next]});
            ring.erase(ring.begin() + static_cast<std::ptrdiff_t>(i));
            clipped = true;
            break;
        }

        if (!clipped)
        {
            // No ear anywhere: the ring is self-intersecting or degenerate, and
            // no decomposition of it is meaningful. The remainder still becomes
            // collision, as its hull — say so rather than shipping it silently.
            Logger::Log::Warning(
                "[TileMap] Tile {} has a collision polygon that cannot be decomposed "
                "(self-intersecting or degenerate); it will collide as its convex hull",
                tileId);
            break;
        }
    }

    if (ring.size() >= 3)
        outPieces.push_back(std::move(ring));
}

/**
 * One tile's collision geometry in tile-local space, resolved once per chunk
 * build rather than once per cell.
 */
struct TileCollisionPieces
{
    // The tile fills its cell: it joins the occupancy grid and the merge, and
    // no outline is emitted for it.
    bool FullSquare = false;
    std::vector<Ring> Pieces;
};

TileCollisionPieces DecomposeTile(const TileSetTile& tile)
{
    TileCollisionPieces result;

    for (const TileCollisionPolygon& polygon : tile.CollisionPolygons)
    {
        if (polygon.Points.size() >= 3 && IsFullSquarePolygon(polygon))
        {
            result.FullSquare = true;
            break;
        }
    }

    if (result.FullSquare)
    {
        // A full square already covers every point of the cell, so any further
        // polygon can only add an overlapping second shape over the same area.
        if (tile.CollisionPolygons.size() > 1)
        {
            Logger::Log::Warning(
                "[TileMap] Tile {} carries a full-square collision polygon and {} more; the "
                "square covers the cell, so the others are ignored",
                tile.Id, tile.CollisionPolygons.size() - 1);
        }
        return result;
    }

    for (const TileCollisionPolygon& polygon : tile.CollisionPolygons)
    {
        if (polygon.Points.size() < 3)
            continue;
        SplitToConvexPieces(polygon.Points, tile.Id, result.Pieces);
    }
    return result;
}

} // namespace

bool IsFullSquarePolygon(const TileCollisionPolygon& polygon)
{
    if (polygon.Points.size() != 4)
        return false;

    std::array<bool, 4> seen{}; // (0,0) (1,0) (1,1) (0,1)
    for (const Mathematics::Vector2& p : polygon.Points)
    {
        if (IsCorner(p, 0.0f, 0.0f))
            seen[0] = true;
        else if (IsCorner(p, 1.0f, 0.0f))
            seen[1] = true;
        else if (IsCorner(p, 1.0f, 1.0f))
            seen[2] = true;
        else if (IsCorner(p, 0.0f, 1.0f))
            seen[3] = true;
        else
            return false;
    }
    return seen[0] && seen[1] && seen[2] && seen[3];
}

Mathematics::Vector2 TransformTileLocalPoint(Mathematics::Vector2 point, TileCell cell)
{
    if (CellFlipV(cell))
        point.y = 1.0f - point.y;
    if (CellFlipH(cell))
        point.x = 1.0f - point.x;
    if (CellTranspose(cell))
        std::swap(point.x, point.y);
    return point;
}

ChunkCollisionShapes BuildChunkCollisionShapes(const TileMapChunk& chunk,
                                               const TileSetAsset& tileSet)
{
    ChunkCollisionShapes shapes;

    // First-encountered order over the row-major cell scan, so the emitted
    // shape order is deterministic for a given chunk.
    std::vector<MergeGroup> groups;
    auto groupFor = [&groups](TileCollisionMode mode, const GUID& material) -> MergeGroup&
    {
        for (MergeGroup& group : groups)
            if (group.Mode == mode && group.Material == material)
                return group;
        MergeGroup& group = groups.emplace_back();
        group.Mode = mode;
        group.Material = material;
        return group;
    };

    std::unordered_map<const TileSetTile*, TileCollisionPieces> tilePieces;

    for (int32 y = 0; y < kChunkSize; ++y)
    {
        for (int32 x = 0; x < kChunkSize; ++x)
        {
            const TileCell cell = chunk.Cells[static_cast<size_t>(y * kChunkSize + x)];
            if (CellIsEmpty(cell))
                continue;
            const TileSetTile* tile = tileSet.FindTile(CellTileId(cell));
            if (!tile)
                continue;

            auto decomposed = tilePieces.find(tile);
            if (decomposed == tilePieces.end())
                decomposed = tilePieces.emplace(tile, DecomposeTile(*tile)).first;

            if (decomposed->second.FullSquare)
            {
                // A full square is orientation-invariant; one occupancy bit is
                // the whole cell.
                groupFor(tile->CollisionMode, tile->PhysicsMaterial)
                    .Occupancy.set(static_cast<size_t>(y * kChunkSize + x));
                continue;
            }

            for (const Ring& piece : decomposed->second.Pieces)
            {
                CollisionOutline outline;
                outline.Mode = tile->CollisionMode;
                outline.Material = tile->PhysicsMaterial;
                outline.Points.reserve(piece.size());
                for (const Mathematics::Vector2& p : piece)
                {
                    const Mathematics::Vector2 local = TransformTileLocalPoint(p, cell);
                    outline.Points.push_back(Mathematics::Vector2(
                        static_cast<float32>(x) + local.x, static_cast<float32>(y) + local.y));
                }
                shapes.Outlines.push_back(std::move(outline));
            }
        }
    }

    for (const MergeGroup& group : groups)
        MergeOccupancyToRects(group, shapes.Rects);

    return shapes;
}

} // namespace TileMap
} // namespace GameEngine
