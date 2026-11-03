#pragma once

#include "AssetCore/GUID.h"
#include "Physics/PhysicsMaterial.h"
#include "TileMap/TileSetAsset.h"
#include "Types/Types.h"

namespace GameEngine
{
namespace TileMapECS
{

/**
 * @brief What one cell's tile says about standing on it.
 *
 * The physics-side answer to the same question arrives through contacts, which
 * a character doing its own kinematic sweep never generates. This is that
 * character's route to the identical authored data.
 */
struct TileSurface
{
    /** The cell holds a tile at all. */
    bool HasTile = false;

    /** That tile authors collision geometry (what the chunk baker turns into shapes). */
    bool HasCollision = false;

    /** Solid, or solid only to something landing on its top face. */
    TileMap::TileCollisionMode Mode = TileMap::TileCollisionMode::Solid;

    /** True when the tile points at a .physmat; false leaves Material at its defaults. */
    bool HasMaterial = false;

    /** The resolved contact surface — friction, restitution and their combine modes. */
    Physics::PhysicsMaterial Material{};

    /** Convenience: the tile is one-way. */
    bool IsOneWay() const { return Mode == TileMap::TileCollisionMode::OneWay; }
};

/**
 * The authored surface of one cell, resolving the tile through the layer's
 * tileset and the tile's material through the engine's contact-surface table.
 *
 * An empty cell, an unknown map/layer, or a tileset that has not loaded yet all
 * return a default TileSurface (HasTile false). Main thread only — it can load
 * the tileset and the .physmat on first use.
 *
 * Whether a tile *blocks* is the caller's policy, not this function's: a
 * platformer that treats every painted cell as ground reads HasTile, one that
 * follows the authored shapes reads HasCollision. Mode and Material mean the
 * same thing either way.
 */
TileSurface QueryTileSurface(const GUID& mapGuid, uint32 layerId, int32 cellX, int32 cellY);

} // namespace TileMapECS
} // namespace GameEngine
