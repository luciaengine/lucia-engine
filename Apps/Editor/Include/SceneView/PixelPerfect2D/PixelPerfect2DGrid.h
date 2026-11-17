#pragma once

#include <cstdint>

namespace GameEngine::Editor
{

/** Pixel-perfect zoom steps the Scene View offers: 1x, 2x, 4x ... 64x. */
inline constexpr int32_t kPixelPerfectScaleMin = 1;
inline constexpr int32_t kPixelPerfectScaleMax = 64;

/** Texel density for a scene with no tile map: one world unit is one source texel. */
inline constexpr float kPixelPerfectDefaultPixelsPerUnit = 1.0f;

/**
 * The Scene View's 2D pixel-perfect grid. The pixel scale counts render pixels per source
 * texel, not per world unit: at 1x, one tile set texel covers one render pixel.
 */
struct PixelPerfect2DGrid
{
    /** World units that one source texel spans (1 / pixels per unit). */
    float WorldUnitsPerTexel = 1.0f;
    /** Orthographic visible height in world units; 0 when the render height is 0. */
    float OrthoHeight = 0.0f;
};

/**
 * The grid that puts one source texel on `pixelScale` render pixels. `renderHeightPx` is in
 * render-target pixels, not layout points. A density that is not positive and finite uses
 * kPixelPerfectDefaultPixelsPerUnit; the scale is clamped to the offered steps.
 */
PixelPerfect2DGrid ComputePixelPerfect2DGrid(uint32_t renderHeightPx, int32_t pixelScale, float pixelsPerUnit);

/** The largest power-of-two pixel scale whose visible height still covers `requestedHeight` world units. */
int32_t SnapPixelPerfectScaleToHeight(uint32_t renderHeightPx, float requestedHeight, float pixelsPerUnit);

/**
 * Moves an orthographic window's center so that its minimum edge lands on a texel boundary.
 * The minimum edge, not the center, is snapped: a window of an odd texel count has its center
 * on a half texel. A texel size that is not positive leaves the center unchanged.
 */
void SnapOrthoWindowToTexelGrid(double& centerX, double& centerY, float halfWidth, float halfHeight,
                                float worldUnitsPerTexel);

/** Steps `scale` by `steps` doublings (positive) or halvings (negative), clamped to the offered steps. */
int32_t StepPixelPerfectScale(int32_t scale, int32_t steps);

} // namespace GameEngine::Editor
