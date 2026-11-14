#pragma once

#include <vulkan/vulkan.h>

namespace GameEngine::Rendering
{
// Android API 28 exposes the Vulkan 1.1 loader. Promoted 1.2 commands must be
// resolved on the logical device, including their extension names on 1.1.
struct VulkanPromotedCommands
{
    PFN_vkGetBufferDeviceAddress GetBufferDeviceAddress = nullptr;
    PFN_vkCmdDrawIndexedIndirectCount CmdDrawIndexedIndirectCount = nullptr;
    PFN_vkCreateRenderPass2 CreateRenderPass2 = nullptr;
    PFN_vkCmdBeginRenderPass2 CmdBeginRenderPass2 = nullptr;
    PFN_vkCmdEndRenderPass2 CmdEndRenderPass2 = nullptr;
    PFN_vkGetSemaphoreCounterValue GetSemaphoreCounterValue = nullptr;
    PFN_vkWaitSemaphores WaitSemaphores = nullptr;

    template<typename Function>
    static Function Resolve(VkDevice device, const char* core, const char* extension)
    {
        auto function = vkGetDeviceProcAddr(device, core);
        if (function == nullptr) function = vkGetDeviceProcAddr(device, extension);
        return reinterpret_cast<Function>(function);
    }

    void Initialize(VkDevice device)
    {
        GetBufferDeviceAddress = Resolve<PFN_vkGetBufferDeviceAddress>(device, "vkGetBufferDeviceAddress", "vkGetBufferDeviceAddressKHR");
        CmdDrawIndexedIndirectCount = Resolve<PFN_vkCmdDrawIndexedIndirectCount>(device, "vkCmdDrawIndexedIndirectCount", "vkCmdDrawIndexedIndirectCountKHR");
        CreateRenderPass2 = Resolve<PFN_vkCreateRenderPass2>(device, "vkCreateRenderPass2", "vkCreateRenderPass2KHR");
        CmdBeginRenderPass2 = Resolve<PFN_vkCmdBeginRenderPass2>(device, "vkCmdBeginRenderPass2", "vkCmdBeginRenderPass2KHR");
        CmdEndRenderPass2 = Resolve<PFN_vkCmdEndRenderPass2>(device, "vkCmdEndRenderPass2", "vkCmdEndRenderPass2KHR");
        GetSemaphoreCounterValue = Resolve<PFN_vkGetSemaphoreCounterValue>(device, "vkGetSemaphoreCounterValue", "vkGetSemaphoreCounterValueKHR");
        WaitSemaphores = Resolve<PFN_vkWaitSemaphores>(device, "vkWaitSemaphores", "vkWaitSemaphoresKHR");
    }
};
}
