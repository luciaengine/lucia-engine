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
    uint32 LayerId = 0;

    uint32 RenderLayers = 0xFFFFFFFFu;
    bool Visible = true;

    float32 Opacity = 1.0f;

    /** Multiplied into every tile's texel color. 0xRRGGBBAA. */
    uint32 TintRGBA = 0xFFFFFFFFu;

    /**
     * Fraction of camera motion this layer follows: 1 = locked to the world,
     * 0 = locked to the camera. Applied per axis in the tilemap's local plane.
     */
    float32 ParallaxX = 1.0f;
    float32 ParallaxY = 1.0f;

    /** Local-space depth offset, so layers on one entity sort against each other. */
    float32 ZOffset = 0.0f;
};

} // namespace Components
} // namespace GameEngine
