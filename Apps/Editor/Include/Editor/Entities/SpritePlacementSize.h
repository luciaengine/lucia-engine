#pragma once

#include <cmath>

namespace GameEngine::Editor
{

struct SpritePlacementSize
{
    float Width = 1.0f;
    float Height = 1.0f;
};

// Static texture drops use the same density as the target scene's tile grid.
// A scene without one sizes them at one unit per pixel.
inline SpritePlacementSize ResolveSpritePlacementSize(float widthPx, float heightPx,
                                                       float scenePixelsPerUnit)
{
    const auto positiveFinite = [](float value) { return std::isfinite(value) && value > 0.0f; };
    const float density = positiveFinite(scenePixelsPerUnit) ? scenePixelsPerUnit : 1.0f;
    const float width = widthPx / density;
    const float height = heightPx / density;
    if (!positiveFinite(widthPx) || !positiveFinite(heightPx) ||
        !positiveFinite(width) || !positiveFinite(height))
        return {};
    return {width, height};
}

} // namespace GameEngine::Editor
