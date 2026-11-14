#pragma once

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

    bool IsValid() const
    {
        constexpr uint32_t kMaxResolution = kVirtualLocalShadowPagesPerAxis * kVirtualShadowPageTexels;
        return std::has_single_bit(DirectionalResolution) && DirectionalResolution >= kVirtualShadowPageTexels &&
               DirectionalResolution <= kMaxResolution && std::has_single_bit(LocalResolution) &&
               LocalResolution >= kVirtualShadowPageTexels && LocalResolution <= kMaxResolution;
    }

    bool operator==(const VirtualShadowOptions&) const = default;
};
} // namespace GameEngine::Engine::Renderer
