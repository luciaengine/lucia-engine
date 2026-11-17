#pragma once

#include "Mathematics/Matrix4x4.h"
#include "Mathematics/Vector4.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace GameEngine::Engine::Renderer
{

inline constexpr uint32_t kVirtualShadowDirectionalRows = 4;
inline constexpr uint32_t kVirtualShadowClusterLightRows = 1024;

struct alignas(16) VirtualShadowLookupHeaderGPU
{
    std::array<uint32_t, kVirtualShadowDirectionalRows> DirectionalRows;
    std::array<uint32_t, kVirtualShadowClusterLightRows> LocalRows;
};
static_assert(sizeof(VirtualShadowLookupHeaderGPU) == 4112);

struct alignas(16) VirtualShadowLookupProjectionGPU
{
    Mathematics::Matrix4x4 RelativeToLightClip;
    uint32_t FirstPage;
    uint32_t PagesPerAxis;
    uint32_t LastMip;
    uint32_t Reserved;
    // xyz = relative light position and w = far plane for local lights.
    // x = world units per texel and y = light depth span for directional levels.
    Mathematics::Vector4 LightPositionRelAndFar;
};
static_assert(sizeof(VirtualShadowLookupProjectionGPU) == 96);
static_assert(offsetof(VirtualShadowLookupProjectionGPU, FirstPage) == 64);
static_assert(offsetof(VirtualShadowLookupProjectionGPU, LightPositionRelAndFar) == 80);

} // namespace GameEngine::Engine::Renderer
