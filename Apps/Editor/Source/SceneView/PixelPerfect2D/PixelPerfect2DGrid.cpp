#include "SceneView/PixelPerfect2D/PixelPerfect2DGrid.h"

#include <algorithm>
#include <cmath>

namespace GameEngine::Editor
{
namespace
{

float UsableDensity(float pixelsPerUnit)
{
    return (pixelsPerUnit > 0.0f && std::isfinite(pixelsPerUnit)) ? pixelsPerUnit : kPixelPerfectDefaultPixelsPerUnit;
}

} // namespace

PixelPerfect2DGrid ComputePixelPerfect2DGrid(uint32_t renderHeightPx, int32_t pixelScale, float pixelsPerUnit)
{
    PixelPerfect2DGrid grid{};
    grid.WorldUnitsPerTexel = 1.0f / UsableDensity(pixelsPerUnit);
    if (renderHeightPx == 0)
        return grid;
    const int32_t scale = std::clamp(pixelScale, kPixelPerfectScaleMin, kPixelPerfectScaleMax);
    grid.OrthoHeight = static_cast<float>(renderHeightPx) * grid.WorldUnitsPerTexel / static_cast<float>(scale);
    return grid;
}

int32_t SnapPixelPerfectScaleToHeight(uint32_t renderHeightPx, float requestedHeight, float pixelsPerUnit)
{
    if (renderHeightPx == 0 || !(requestedHeight > 0.0f))
        return kPixelPerfectScaleMin;
    // The visible height at scale s is renderHeightPx / (s * density).
    const float ratio = static_cast<float>(renderHeightPx) / (requestedHeight * UsableDensity(pixelsPerUnit));
    int32_t scale = kPixelPerfectScaleMin;
    while (static_cast<float>(scale * 2) <= ratio && scale < kPixelPerfectScaleMax)
        scale *= 2;
    return scale;
}

void SnapOrthoWindowToTexelGrid(double& centerX, double& centerY, float halfWidth, float halfHeight,
                                float worldUnitsPerTexel)
{
    if (!(worldUnitsPerTexel > 0.0f))
        return;
    const double texel = static_cast<double>(worldUnitsPerTexel);
    const double halfW = static_cast<double>(halfWidth);
    const double halfH = static_cast<double>(halfHeight);
    centerX = std::round((centerX - halfW) / texel) * texel + halfW;
    centerY = std::round((centerY - halfH) / texel) * texel + halfH;
}

int32_t StepPixelPerfectScale(int32_t scale, int32_t steps)
{
    int32_t result = std::clamp(scale, kPixelPerfectScaleMin, kPixelPerfectScaleMax);
    for (int32_t i = 0; i < steps && result < kPixelPerfectScaleMax; ++i)
        result *= 2;
    for (int32_t i = 0; i > steps && result > kPixelPerfectScaleMin; --i)
        result /= 2;
    return std::clamp(result, kPixelPerfectScaleMin, kPixelPerfectScaleMax);
}

} // namespace GameEngine::Editor
