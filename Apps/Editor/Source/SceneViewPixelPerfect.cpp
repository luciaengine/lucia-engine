#include "Editor/SceneViewPixelPerfect.h"

#include <algorithm>
#include <cmath>

namespace GameEngine
{
namespace Editor
{

PixelPerfect2DGrid ComputePixelPerfect2DGrid(uint32_t renderHeightPx, int32_t pixelScale,
                                             float pixelsPerUnit)
{
    PixelPerfect2DGrid grid{};
    const float density =
        (pixelsPerUnit > 0.0f && std::isfinite(pixelsPerUnit)) ? pixelsPerUnit : kPixelPerfectDefaultPixelsPerUnit;
    grid.WorldUnitsPerTexel = 1.0f / density;

    const int32_t scale = std::clamp(pixelScale, kPixelPerfectScaleMin, kPixelPerfectScaleMax);
    if (renderHeightPx == 0)
        return grid;

    grid.OrthoHeight = static_cast<float>(renderHeightPx) * grid.WorldUnitsPerTexel /
                       static_cast<float>(scale);
    return grid;
}

int32_t SnapPixelPerfectScaleToHeight(uint32_t renderHeightPx, float requestedHeight,
                                      float pixelsPerUnit)
{
    if (renderHeightPx == 0 || !(requestedHeight > 0.0f))
        return kPixelPerfectScaleMin;

    const float density =
        (pixelsPerUnit > 0.0f && std::isfinite(pixelsPerUnit)) ? pixelsPerUnit : kPixelPerfectDefaultPixelsPerUnit;
    // Visible height at scale s is renderHeightPx / (s * density); the largest
    // s that still covers requestedHeight is the most zoomed-in step that fits.
    const float ratio = static_cast<float>(renderHeightPx) / (requestedHeight * density);
    int32_t scale = kPixelPerfectScaleMin;
    while (static_cast<float>(scale * 2) <= ratio && scale < kPixelPerfectScaleMax)
        scale *= 2;
    return std::clamp(scale, kPixelPerfectScaleMin, kPixelPerfectScaleMax);
}

void SnapOrthoWindowToTexelGrid(double& centerX, double& centerY, float halfWidth,
                                float halfHeight, float worldUnitsPerTexel)
{
    if (!(worldUnitsPerTexel > 0.0f))
        return;
    const double texel = static_cast<double>(worldUnitsPerTexel);
    const double halfW = static_cast<double>(halfWidth);
    const double halfH = static_cast<double>(halfHeight);
    centerX = std::round((centerX - halfW) / texel) * texel + halfW;
    centerY = std::round((centerY - halfH) / texel) * texel + halfH;
}

} // namespace Editor
} // namespace GameEngine
