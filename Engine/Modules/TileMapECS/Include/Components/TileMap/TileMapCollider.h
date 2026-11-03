#pragma once

#include "Types/Types.h"

namespace GameEngine
{
namespace Components
{

/**
 * @brief Opts an entity's TileMapLayerRef layers into static physics.
 *
 * Presence is the opt-in: without this component a tilemap is render-only.
 * TileMapCollisionSystem bakes the referenced layers' tile collision polygons
 * into per-chunk static bodies at the entity's transform.
 */
struct TileMapCollider
{
    /** Contact friction of the baked tile bodies (0 = frictionless). */
    float32 Friction = 0.5f;

    /** Contact restitution of the baked tile bodies (0 = no bounce). */
    float32 Restitution = 0.0f;
};

} // namespace Components
} // namespace GameEngine
