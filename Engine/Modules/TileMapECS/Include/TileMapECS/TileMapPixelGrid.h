#pragma once

#include "AssetCore/Types.h"
#include <cmath>

namespace GameEngine
{
namespace TileMapECS
{

/**
 * Texel density no tilemap could supply: no visible layer, no tileset, or a
 * degenerate cell size. Callers treat it as "keep whatever the user authored"
 * rather than substituting a density of their own.
 */
inline constexpr float32 kUnresolvedPixelsPerUnit = 0.0f;

/**
 * @brief Source texels per world unit for a tileset drawn on a given cell size.
 *
 * A 16 px tile on a one-unit cell is 16 pixels per unit; the same tile on a
 * two-unit cell is 8. This is the density the pixel-perfect camera maps onto
 * screen pixels, so it must come from the art (tile pixels) and the world grid
 * (cell size) together — either alone is meaningless.
 *
 * Returns kUnresolvedPixelsPerUnit for a non-positive tile width or cell size
 * instead of dividing, so a half-loaded tileset yields "unknown" rather than a
 * density of zero or infinity that a camera would then render at.
 */
inline float32 DerivePixelsPerUnit(int32 tilePixelWidth, float32 cellWidth)
{
    if (tilePixelWidth <= 0 || !(cellWidth > 0.0f))
        return kUnresolvedPixelsPerUnit;
    const float32 density = static_cast<float32>(tilePixelWidth) / cellWidth;
    return std::isfinite(density) && density > 0.0f ? density : kUnresolvedPixelsPerUnit;
}

} // namespace TileMapECS
} // namespace GameEngine
