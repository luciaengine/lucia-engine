#pragma once

#include "Sprite2D/SpriteFrames.h"

#include <cstddef>

namespace GameEngine
{
namespace Sprite2D
{

/** Returned by SpriteFrameIndexAt when the animation holds no frames. */
inline constexpr std::size_t kInvalidSpriteFrameIndex = static_cast<std::size_t>(-1);

/** Wall-clock length of one pass through the animation, in seconds. */
float32 SpriteAnimationDuration(const SpriteFrameAnimation& animation);

/**
 * @brief Folds a raw clock into the animation's own range.
 *
 * Looping animations wrap (negative times wrap forward, so a reversed
 * speedScale plays backwards through the strip); one-shots clamp to [0, duration]
 * and therefore hold on the final frame. A zero-length animation yields 0.
 */
float32 NormalizeSpriteTime(const SpriteFrameAnimation& animation, float32 timeSeconds);

/**
 * @brief Advances a sprite clock by `deltaSeconds * speedScale` and normalizes it.
 *
 * The result is always in range, so the stored component time never accumulates
 * without bound — a sprite alive for hours keeps the same frame-timing precision
 * it had on its first second.
 */
float32 AdvanceSpriteTime(const SpriteFrameAnimation& animation, float32 timeSeconds,
                          float32 deltaSeconds, float32 speedScale);

/**
 * @brief Index of the frame visible at `timeSeconds` (normalized internally).
 *
 * Frame i covers (start_i, start_i + duration_i]; the first frame also owns
 * t == 0. kInvalidSpriteFrameIndex when the animation has no frames.
 */
std::size_t SpriteFrameIndexAt(const SpriteFrameAnimation& animation, float32 timeSeconds);

} // namespace Sprite2D
} // namespace GameEngine
