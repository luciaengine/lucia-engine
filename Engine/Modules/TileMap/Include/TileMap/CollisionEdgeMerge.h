#pragma once

#include "Mathematics/Vector2.h"
#include "TileMap/TileMapChunk.h"
#include "TileMap/TileSetAsset.h"

#include <vector>

namespace GameEngine
{
namespace TileMap
{

/**
 * @brief Axis-aligned collision rectangle in chunk-local cell space (unit = one
 * cell, origin = the chunk's minimum corner, +Y up).
 */
struct CollisionRect
{
    float32 MinX = 0.0f;
    float32 MinY = 0.0f;
    float32 MaxX = 0.0f;
    float32 MaxY = 0.0f;
    TileCollisionMode Mode = TileCollisionMode::Solid;
    GUID Material; // null => the collider's own friction/restitution

    bool operator==(const CollisionRect&) const = default;
};

/**
 * Collision outline in chunk-local cell space (same frame as CollisionRect).
 *
 * Always convex: the baker extrudes each outline through a convex hull, so an
 * authored concave polygon is split into convex pieces before it gets here.
 */
struct CollisionOutline
{
    std::vector<Mathematics::Vector2> Points;
    TileCollisionMode Mode = TileCollisionMode::Solid;
    GUID Material; // null => the collider's own friction/restitution
};

/** Merged collision geometry of one chunk. */
struct ChunkCollisionShapes
{
    std::vector<CollisionRect> Rects;
    std::vector<CollisionOutline> Outlines;

    bool IsEmpty() const { return Rects.empty() && Outlines.empty(); }
};

/** True when the polygon is exactly the unit square (any point order). */
bool IsFullSquarePolygon(const TileCollisionPolygon& polygon);

/**
 * Maps a tile-local collision point ([0,1]², +Y up, authored over the unflipped
 * tile art) into the cell's geometry frame. The cell's flip bits transform the
 * *texture lookup* over a fixed quad (Tiled GID order: transpose, then flip H,
 * then flip V), so collision points move by the inverse: flip V, then flip H,
 * then transpose.
 */
Mathematics::Vector2 TransformTileLocalPoint(Mathematics::Vector2 point, TileCell cell);

/**
 * Merged collision geometry for one chunk, in chunk-local cell space.
 *
 * Cells whose tile carries a full-square collision polygon enter an occupancy
 * grid that a greedy row-scan merge reduces to maximal rectangles. Cells are
 * grouped by (collision mode, physics material) and merge only within a group,
 * so a one-way or icy tile never gets absorbed into a neighbouring solid
 * rectangle: one rectangle carries one mode and one material. The square covers
 * the whole cell, so any further polygon on that tile is redundant and is
 * dropped with a warning rather than overlapping a second shape onto it.
 *
 * Non-square polygons never merge: each is split into convex pieces and emitted
 * per cell, transformed by the cell's flip/transpose bits and translated to the
 * cell's chunk-local origin.
 */
ChunkCollisionShapes BuildChunkCollisionShapes(const TileMapChunk& chunk,
                                               const TileSetAsset& tileSet);

} // namespace TileMap
} // namespace GameEngine
