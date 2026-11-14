#pragma once

#include "Types/Types.h"
#include "Components/Transform.h"

namespace GameEngine::Engine::Renderer
{

/**
 * @brief World-space frame of a sprite quad: the quad spans
 * Center ± AxisX ± AxisY, so AxisX/AxisY are half-extent vectors.
 */
struct SpriteQuadPlacement
{
    Mathematics::WorldPosition Center;
    float32 AxisX[3] = {};
    float32 AxisY[3] = {};
};

/**
 * @brief Places a frame's quad so its pivot lands exactly on the entity's
 * world position.
 *
 * The pivot is normalized frame space with (0,0) at the frame's bottom-left and
 * (1,1) at its top-right: (0.5, 0.5) centers the quad, (0.5, 0) anchors it by
 * the feet. Anchoring by the feet is what makes a frame-size change (an attack
 * pose taller than an idle pose) leave the visible feet where they were —
 * the quad grows upward instead of straddling the entity origin.
 *
 * Flips mirror the frame about the PIVOT, not the quad centre, by reflecting the
 * pivot's normalized coordinate. Mirroring about the centre would slide a
 * character sideways by twice its pivot offset every time it turned around.
 * The caller still swaps the UV rect's edges so the image itself mirrors.
 *
 * The transform carries its exact world position and the rotated, scaled local X/Y axes.
 */
inline SpriteQuadPlacement BuildSpriteQuad(const Components::WorldTransform& transform, float32 frameWidthUnits,
                                           float32 frameHeightUnits, float32 pivotX, float32 pivotY,
                                           bool flipX, bool flipY)
{
    const float32 halfW = 0.5f * frameWidthUnits;
    const float32 halfH = 0.5f * frameHeightUnits;

    const float32 effectivePivotX = flipX ? 1.0f - pivotX : pivotX;
    const float32 effectivePivotY = flipY ? 1.0f - pivotY : pivotY;

    // Offset from the pivot to the quad centre, in half-extent multiples:
    // +1 at pivot 0 (centre sits one half-extent along the axis), 0 at 0.5.
    const float32 centreOffsetX = 1.0f - 2.0f * effectivePivotX;
    const float32 centreOffsetY = 1.0f - 2.0f * effectivePivotY;

    SpriteQuadPlacement placement{};
    for (int32 i = 0; i < 3; ++i)
    {
        placement.AxisX[i] = transform.Basis[0][i] * halfW;
        placement.AxisY[i] = transform.Basis[1][i] * halfH;

    }
    placement.Center = transform.GetPosition() + Mathematics::Vector3{
        placement.AxisX[0] * centreOffsetX + placement.AxisY[0] * centreOffsetY,
        placement.AxisX[1] * centreOffsetX + placement.AxisY[1] * centreOffsetY,
        placement.AxisX[2] * centreOffsetX + placement.AxisY[2] * centreOffsetY};
    return placement;
}

} // namespace GameEngine::Engine::Renderer
