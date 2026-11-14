#pragma once

#include "../../Rendering/Source/Vulkan/VulkanDevice/VulkanDevice.h"

#include <limits>

namespace GameEngine::CBTTerrain::Test
{
class SourceCompletionProbeDevice : public Rendering::VulkanDevice
{
  public:
    bool HideGraphicsToken = false;
    bool UseScopedGraphicsFence = false;
    uint64_t RejectCompletionAfter = std::numeric_limits<uint64_t>::max();
    bool RefuseNextGraphicsSubmit = false;
    bool ReportFailureAfterGraphicsSubmit = false;
    GpuSyncToken LastAttemptedSignal{};

    Rendering::GraphicsAPI GetAPI() const override
    {
        return UseScopedGraphicsFence ? Rendering::GraphicsAPI::Metal : Rendering::VulkanDevice::GetAPI();
    }

    GpuSyncToken LastGraphicsSubmissionToken() const override
    {
        const auto token = Rendering::VulkanDevice::LastGraphicsSubmissionToken();
        return HideGraphicsToken || UseScopedGraphicsFence ? GpuSyncToken{} : token;
    }

    bool WaitTimelineSemaphoreValue(Rendering::SemaphoreHandle semaphore, uint64_t value, uint64_t timeout) override
    {
        if (Rendering::VulkanDevice::LastGraphicsSubmissionToken().value > RejectCompletionAfter)
        {
            RejectCompletionAfter = std::numeric_limits<uint64_t>::max();
            return false;
        }
        return Rendering::VulkanDevice::WaitTimelineSemaphoreValue(semaphore, value, timeout);
    }

    void ExecuteCommandLists(const std::vector<Rendering::CommandList*>& commands) override
    {
        if (RefuseNextGraphicsSubmit)
        {
            RefuseNextGraphicsSubmit = false;
            return;
        }
        Rendering::VulkanDevice::ExecuteCommandLists(commands);
    }

    bool QueueSubmit(QueueType queue, const std::vector<Rendering::CommandList*>& commands,
        const std::vector<std::pair<Rendering::SemaphoreHandle, uint64_t>>& waits,
        const std::vector<std::pair<Rendering::SemaphoreHandle, uint64_t>>& signals) override
    {
        if (queue == QueueType::Graphics && !signals.empty())
            LastAttemptedSignal = {signals.back().first, signals.back().second};
        if (queue == QueueType::Graphics && RefuseNextGraphicsSubmit)
        {
            RefuseNextGraphicsSubmit = false;
            return false;
        }
        const bool submitted = Rendering::VulkanDevice::QueueSubmit(queue, commands, waits, signals);
        if (queue == QueueType::Graphics && submitted && ReportFailureAfterGraphicsSubmit)
        {
            ReportFailureAfterGraphicsSubmit = false;
            return false;
        }
        return submitted;
    }
};
} // namespace GameEngine::CBTTerrain::Test
