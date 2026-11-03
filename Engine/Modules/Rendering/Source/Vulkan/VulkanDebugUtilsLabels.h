/**
 * @file VulkanDebugUtilsLabels.h
 * @brief GPU debug labels and object names via VK_EXT_debug_utils.
 */

#pragma once

#include <vulkan/vulkan.h>

namespace GameEngine {
namespace Rendering {

    /**
     * @brief Cached VK_EXT_debug_utils command entry points.
     *
     * Available() requires all three: a half-resolved set opens labels it cannot
     * close, and an unbalanced label tree misleads every downstream tool that
     * reads it.
     */
    struct DebugUtilsLabelFns
    {
        PFN_vkCmdBeginDebugUtilsLabelEXT  Begin  = nullptr;
        PFN_vkCmdEndDebugUtilsLabelEXT    End    = nullptr;
        PFN_vkCmdInsertDebugUtilsLabelEXT Insert = nullptr;

        bool Available() const noexcept { return Begin && End && Insert; }
    };

} // namespace Rendering
} // namespace GameEngine
