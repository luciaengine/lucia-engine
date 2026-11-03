#pragma once

#include <cstdint>

namespace GameEngine
{
namespace Editor
{

/** Pixel-perfect zoom steps the Scene View offers (1x, 2x, 4x ... 64x). */
inline constexpr int32_t kPixelPerfectScaleMin = 1;
inline constexpr int32_t kPixelPerfectScaleMax = 64;

/**
 * Texel density assumed when a scene declares none: one world unit is one
 * source texel, so the pixel scale keeps its "screen pixels per world unit"
 * meaning for art that carries no pixel grid.
 */
inline constexpr float kPixelPerfectDefaultPixelsPerUnit = 1.0f;

/**
 * The Scene View's 2D pixel-perfect grid.
 *
 * The pixel scale counts render pixels per SOURCE TEXEL, not per world unit: a
 * 16 px tile authored on a one-unit cell has 16 pixels per unit, so 1x must put
 * one atlas texel in one render pixel — treating the scale as pixels per unit
 * instead renders that tilemap at 1/16 scale.
 */
struct PixelPerfect2DGrid
{
    /** World units spanned by one source texel (1 / pixels-per-unit). */
    float WorldUnitsPerTexel = 1.0f;
    /** Orthographic visible height in world units; 0 when the render height is unknown. */
    float OrthoHeight = 0.0f;
};

/**
 * Ortho height that maps one source texel onto exactly `pixelScale` render
 * pixels vertically. `renderHeightPx` is render-target pixels (DPI-scaled), not
 * layout points — pixel-perfect is a property of the framebuffer grid.
 */
PixelPerfect2DGrid ComputePixelPerfect2DGrid(uint32_t renderHeightPx, int32_t pixelScale,
                                             float pixelsPerUnit);

/**
 * Largest power-of-two pixel scale whose visible height still covers
 * `requestedHeight` world units, so framing a selection lands on a pixel-perfect
 * step instead of an arbitrary zoom.
 */
int32_t SnapPixelPerfectScaleToHeight(uint32_t renderHeightPx, float requestedHeight,
                                      float pixelsPerUnit);

/**
 * Shift an orthographic window's center so its minimum edge lands on a texel
 * boundary. Snapping the CENTER instead would misalign the grid whenever the
 * window spans an odd number of texels; the minimum edge is parity-free.
 */
void SnapOrthoWindowToTexelGrid(double& centerX, double& centerY, float halfWidth,
                                float halfHeight, float worldUnitsPerTexel);

} // namespace Editor
} // namespace GameEngine
