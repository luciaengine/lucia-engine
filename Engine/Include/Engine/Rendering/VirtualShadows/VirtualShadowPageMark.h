#pragma once

#include "Mathematics/Matrix4x4.h"
#include "Mathematics/Vector4.h"

#include <cstddef>
#include <cstdint>

namespace GameEngine::Engine::Renderer
{

struct alignas(16) VirtualShadowMarkProjectionGPU
{
    // Transform camera clip coordinates into this sun clip level, spot cone,
    // or point cube face. The CPU folds the inverse camera VP into this matrix.
    Mathematics::Matrix4x4 ViewClipToLightClip;
    uint32_t FirstPage;
    uint32_t PagesPerAxis;
    uint32_t LastMip;
    float FilterRadiusUV;
    // Component bounds for a world-space normal offset in light clip space.
    Mathematics::Vector4 NormalBiasClipBounds;
};
static_assert(sizeof(VirtualShadowMarkProjectionGPU) == 96);
static_assert(offsetof(VirtualShadowMarkProjectionGPU, FirstPage) == 64);
static_assert(offsetof(VirtualShadowMarkProjectionGPU, NormalBiasClipBounds) == 80);

struct alignas(16) VirtualShadowMarkParamsGPU
{
    uint32_t Width;
    uint32_t Height;
    uint32_t ProjectionCount;
    uint32_t RequestWordCount;
    uint32_t ViewportX;
    uint32_t ViewportY;
    uint32_t Padding[2];
    // Last row of inverse camera VP: homogeneous world W at a depth sample.
    Mathematics::Vector4 CameraInverseW;
};
static_assert(sizeof(VirtualShadowMarkParamsGPU) == 48);
static_assert(offsetof(VirtualShadowMarkParamsGPU, CameraInverseW) == 32);

} // namespace GameEngine::Engine::Renderer
