#pragma once

#include "Rendering/Camera/CameraTypes.h"
#include "Mathematics/Matrix4x4.h"
#include "Mathematics/Vector4.h"

#include <algorithm>
#include <cmath>

namespace GameEngine::Rendering
{
// Snaps a camera projection to an integer raster grid without changing the
// camera transform. The returned UV transform compensates the snap when the
// raster is displayed at another resolution. It is useful to any low-resolution
// orthographic or fixed-angle render pass, not just a post-process effect.
inline CameraData StabilizeCameraToPixelGrid(const CameraData& camera, uint32_t width, uint32_t height,
                                             float focusDistance, Mathematics::Vector4& uvTransform,
                                             uint32_t guardPixels = 1)
{
    uvTransform = {1, 1, 0, 0};
    if (width == 0 || height == 0) return camera;
    auto result = camera;
    const float sx = float(width) / (width + 2 * guardPixels);
    const float sy = float(height) / (height + 2 * guardPixels);
    float shiftX = 0, shiftY = 0;
    if (focusDistance > 0 && std::abs(camera.proj[0][0]) > 1e-6f && std::abs(camera.proj[1][1]) > 1e-6f)
    {
        const bool perspective = std::abs(camera.proj[3][3]) < 1e-6f;
        const double focus = perspective ? std::max(0.01f, focusDistance) : 1.0;
        const double pixelX = 2.0 * focus / (std::abs(camera.proj[0][0]) * width);
        const double pixelY = 2.0 * focus / (std::abs(camera.proj[1][1]) * height);
        const double x = camera.view[3][0] / pixelX;
        const double y = camera.view[3][1] / pixelY;
        shiftX = static_cast<float>(2.0 * (std::round(x) - x) / width);
        shiftY = static_cast<float>(2.0 * (std::round(y) - y) / height);
    }
    for (int column = 0; column < 4; ++column)
    {
        result.proj[column][0] = sx * (camera.proj[column][0] + shiftX * camera.proj[column][3]);
        result.proj[column][1] = sy * (camera.proj[column][1] + shiftY * camera.proj[column][3]);
    }
    result.viewProj = result.proj * result.view;
    uvTransform = {sx, sy, 0.5f * shiftX * sx, -0.5f * shiftY * sy};
    return result;
}

} // namespace GameEngine::Rendering
