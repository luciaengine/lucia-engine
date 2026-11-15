#pragma once

#include <cstdint>

namespace GameEngine::Engine::Renderer
{

struct alignas(16) VirtualShadowMarkProjectionGPU
{
    // Transform camera clip coordinates into this sun clip level, spot cone,
    // or point cube face. The CPU folds the inverse camera VP into this matrix.
    float ViewClipToLightClip[16];
    uint32_t FirstPage;
    uint32_t PagesPerAxis;
    uint32_t LastMip;
    float FilterRadiusUV;
    // Component bounds for a world-space normal offset in light clip space.
    float NormalBiasClipBounds[4];
};
static_assert(sizeof(VirtualShadowMarkProjectionGPU) == 96);

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
    float CameraInverseW[4];
};
static_assert(sizeof(VirtualShadowMarkParamsGPU) == 48);

} // namespace GameEngine::Engine::Renderer
