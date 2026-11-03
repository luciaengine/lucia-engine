#pragma once

#include "AssetCore/Types.h"

#include <cmath>

namespace GameEngine
{
namespace TileMapECS
{

/** World-space XY translation added to a layer's transform for parallax. */
struct ParallaxOffset
{
    float32 X = 0.0f;
    float32 Y = 0.0f;

    bool operator==(const ParallaxOffset&) const = default;
};

/**
 * @brief World-space offset that makes a layer track the camera by (1 - factor).
 *
 * `factor` is the layer's apparent screen motion as a fraction of a world-locked
 * layer's — the usual parallax convention, in which 1 is a normal foreground and
 * smaller values read as further away. It is applied as a world-space slide of
 * (1 - factor) * camera, which is what the cases below describe:
 *
 *   factor 1 -> offset (0, 0): world-locked. The default, and exactly the
 *               no-parallax result, so a layer left at 1 is byte-identical to
 *               one rendered without this offset at all.
 *   factor 0 -> offset = camera XY: the layer translates with the camera, so it
 *               never moves on screen (skybox-like).
 *
 * Apparent SCREEN motion is therefore proportional to `factor`: moving the
 * camera by d moves the layer by d*(1 - factor) in world space, which is
 * -d*factor relative to the camera. A background at 0.5 slides half as far on
 * screen as a world-locked foreground at 1.0 — the parallax depth cue.
 *
 * Camera XY is the camera's FULL-WORLD position, matching the full-world
 * view-projection the tilemap vertex stage uses. The tilemap path never takes
 * the camera-relative (rebased) route, so the two cannot disagree about which
 * origin they are measured from.
 *
 * The offset is a world-axis slide applied to the layer's world translation,
 * after any rotation, so it never tilts the layer out of its own plane.
 *
 * `worldUnitsPerTexelX/Y` quantize the layer's position RELATIVE TO THE CAMERA
 * to whole texels, so the layer lands on the pixel grid and steps across it in
 * one direction only as the camera moves. Quantizing the world-space slide
 * instead would leave the layer's screen position fractional, and the
 * rasterizer's own rounding of it would fight the quantized steps: the layer
 * flickers back and forth by a texel. Pass 0 to leave the slide continuous.
 * Factor 1 stays exactly (0, 0) either way.
 */
inline ParallaxOffset ComputeParallaxOffset(float32 cameraWorldX, float32 cameraWorldY,
                                            float32 parallaxX, float32 parallaxY,
                                            float32 worldUnitsPerTexelX,
                                            float32 worldUnitsPerTexelY)
{
    const auto slide = [](float32 camera, float32 factor, float32 texel)
    {
        if (factor == 1.0f)
            return 0.0f;
        const float32 screenShift = camera * factor;
        const float32 quantized =
            (texel > 0.0f) ? std::round(screenShift / texel) * texel : screenShift;
        return camera - quantized;
    };
    return ParallaxOffset{slide(cameraWorldX, parallaxX, worldUnitsPerTexelX),
                          slide(cameraWorldY, parallaxY, worldUnitsPerTexelY)};
}

} // namespace TileMapECS
} // namespace GameEngine
