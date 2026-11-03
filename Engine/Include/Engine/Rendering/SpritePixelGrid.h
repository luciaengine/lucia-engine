#pragma once

#include "Types/Types.h"
#include <cmath>

namespace GameEngine::Engine::Renderer
{
// The camera stabilization grid uses the view matrix’s screen right/up rows.
// Move a sprite only in that plane, preserving its depth at rotated cameras.
inline void SnapSpriteToPixelGrid(float32* center, const float32* axisX, const float32* axisY,
                                  float32 worldUnitsPerTexel, const float32* view)
{
    if (!(worldUnitsPerTexel > 0.0f) || !std::isfinite(worldUnitsPerTexel)) return;
    const float32 right[3] = {view[0], view[4], view[8]};
    const float32 up[3] = {view[1], view[5], view[9]};
    const auto dot = [](const float32* a, const float32* b)
    {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    };
    constexpr float32 epsilon = 1e-4f;
    const bool axisAligned = std::abs(dot(axisX, up)) < epsilon &&
                             std::abs(dot(axisY, right)) < epsilon;
    const float32 halfWidth = axisAligned ? std::abs(dot(axisX, right)) : 0.0f;
    const float32 halfHeight = axisAligned ? std::abs(dot(axisY, up)) : 0.0f;
    const float32 minX = dot(center, right) - halfWidth;
    const float32 minY = dot(center, up) - halfHeight;
    const float32 deltaX = std::round(minX / worldUnitsPerTexel) * worldUnitsPerTexel - minX;
    const float32 deltaY = std::round(minY / worldUnitsPerTexel) * worldUnitsPerTexel - minY;
    for (int32 i = 0; i < 3; ++i)
        center[i] += right[i] * deltaX + up[i] * deltaY;
}
} // namespace GameEngine::Engine::Renderer
