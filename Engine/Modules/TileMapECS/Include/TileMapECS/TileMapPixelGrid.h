#pragma once

#include "Types/Types.h"

#include <cmath>

namespace GameEngine
{
namespace ECS
{
class World;
} // namespace ECS

namespace TileMapECS
{

/** Texel density that no tile map supplies. Callers keep the density the user authored. */
inline constexpr float32 kUnresolvedPixelsPerUnit = 0.0f;

/// Source texels per world unit: tile pixel width / cell width (16 px on a 1-unit cell is 16).
/// Returns kUnresolvedPixelsPerUnit when an input is not positive or the result is not finite.
inline float32 DerivePixelsPerUnit(int32 tilePixelWidth, float32 cellWidth)
{
    if (tilePixelWidth <= 0 || !(cellWidth > 0.0f))
        return kUnresolvedPixelsPerUnit;
    const float32 density = static_cast<float32>(tilePixelWidth) / cellWidth;
    return std::isfinite(density) && density > 0.0f ? density : kUnresolvedPixelsPerUnit;
}

/// Texel density of the visible, enabled tile map layer with the lowest (map GUID, layer id), so ECS
/// order cannot change it. Returns kUnresolvedPixelsPerUnit when no such layer resolves to a loaded
/// map and tile set. Main thread only: the tile map store can load assets.
float32 ResolveScenePixelsPerUnit(ECS::World& world);

} // namespace TileMapECS
} // namespace GameEngine
