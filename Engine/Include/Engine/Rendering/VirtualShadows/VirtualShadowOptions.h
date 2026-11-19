#pragma once

#include "Engine/Rendering/VirtualShadows/VirtualShadowFeedback.h"
#include "Engine/Rendering/VirtualShadows/VirtualShadowPageCache.h"

#include <bit>
#include <cstdint>

namespace GameEngine::Engine::Renderer
{
struct VirtualShadowOptions
{
    bool Enabled = false;
    uint32_t DirectionalResolution = 8192;
    uint32_t LocalResolution = 4096;
    /// Physical pages of 128 x 128 texels in each view's pool (4 bytes a texel: 2048 pages = 128 MB).
    /// The device's array layer limit can lower it.
    uint32_t PhysicalPages = 2048;

    bool IsValid() const
    {
        constexpr uint32_t kMaxResolution = kVirtualLocalShadowPagesPerAxis * kVirtualShadowPageTexels;
        return std::has_single_bit(DirectionalResolution) && DirectionalResolution >= kVirtualShadowPageTexels &&
               DirectionalResolution <= kMaxResolution && std::has_single_bit(LocalResolution) &&
               LocalResolution >= kVirtualShadowPageTexels && LocalResolution <= kMaxResolution &&
               PhysicalPages >= kVirtualShadowMinPhysicalPages && PhysicalPages <= kVirtualShadowMaxPhysicalPages;
    }

    bool operator==(const VirtualShadowOptions&) const = default;
};
} // namespace GameEngine::Engine::Renderer
