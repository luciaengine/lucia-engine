#pragma once

#include "../../Rendering/Source/Vulkan/VulkanDevice.h"

#include <limits>

namespace GameEngine::CBTTerrain::Test
{
class SourceCompletionProbeDevice : public Rendering::VulkanDevice
{
  public:
    bool HideGraphicsToken = false;
    bool UseScopedGraphicsFence = false;
    uint64_t HideGraphicsTokensAfter = std::numeric_limits<uint64_t>::max();

    Rendering::GraphicsAPI GetAPI() const override
    {
        return UseScopedGraphicsFence ? Rendering::GraphicsAPI::Metal : Rendering::VulkanDevice::GetAPI();
    }

    GpuSyncToken LastGraphicsSubmissionToken() const override
    {
        const auto token = Rendering::VulkanDevice::LastGraphicsSubmissionToken();
        return HideGraphicsToken || UseScopedGraphicsFence || token.value > HideGraphicsTokensAfter
                   ? GpuSyncToken{} : token;
    }
};
} // namespace GameEngine::CBTTerrain::Test
