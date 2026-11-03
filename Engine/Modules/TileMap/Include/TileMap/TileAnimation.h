#pragma once

#include "TileMap/TileSetAsset.h"

#include <span>

namespace GameEngine
{
namespace TileMap
{

/**
 * Total looping duration of a frame list in seconds. Non-positive frame
 * durations contribute nothing; an empty list (or one that sums to 0) is not
 * an animation.
 */
float32 TotalAnimationDuration(std::span<const TileAnimationFrame> frames);

/**
 * Index of the frame covering `timeSeconds` on the looping timeline. The clock
 * is global and deterministic (every instance of a tile animates in phase —
 * LDtk/Tiled behavior); this is the CPU mirror of the frame walk in
 * tilemap.vert, and the unit under test for it. Returns 0 for a list with no
 * positive-duration frame.
 */
size_t SelectAnimationFrame(std::span<const TileAnimationFrame> frames, float32 timeSeconds);

} // namespace TileMap
} // namespace GameEngine
