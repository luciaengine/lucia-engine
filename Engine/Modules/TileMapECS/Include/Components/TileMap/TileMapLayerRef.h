#pragma once

#include "AssetCore/AssetTypes.h"
#include "Components/AssetRef.h"
#include "Types/Types.h"

namespace GameEngine
{
namespace Components
{

/**
 * @brief Renders one layer of a .tilemap asset at the entity's transform.
 *
 * One component per rendered layer, so layers can carry independent parallax,
 * tint and depth without the tilemap asset knowing about scene placement. Tiles
 * lie in the entity's local XY plane with +Y up.
 */
struct TileMapLayerRef
{
    AssetRef<AssetType::TileMap> TileMap;

    /** Stable layer id within the tilemap asset (0 = unset, nothing renders). */
    // @ge-tooltip Stable id of the layer to draw within the tilemap asset.
    uint32 LayerId = 0;

    uint32 RenderLayers = 0xFFFFFFFFu;
    bool Visible = true;

    // @ge-range 0.0 1.0
    float32 Opacity = 1.0f;

    /** Multiplied into every tile's texel color. 0xRRGGBBAA. */
    // @ge-tooltip 0xRRGGBBAA multiplied into every tile's texel color.
    uint32 TintRGBA = 0xFFFFFFFFu;

    /**
     * Fraction of camera motion this layer follows: 1 = locked to the world,
     * 0 = locked to the camera. Applied per axis in the tilemap's local plane.
     */
    // @ge-range 0.0 2.0
    // @ge-tooltip Fraction of camera motion this layer follows: 1 is world-locked, 0 is pinned to the camera.
    float32 ParallaxX = 1.0f;
    // @ge-range 0.0 2.0
    float32 ParallaxY = 1.0f;

    /** Local-space depth offset, so layers on one entity sort against each other. */
    // @ge-tooltip Local depth offset, so several layers on one entity sort against each other.
    float32 ZOffset = 0.0f;
};

} // namespace Components
} // namespace GameEngine
