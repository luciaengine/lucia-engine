#include <gtest/gtest.h>

#include "../Source/Metal/MetalMappings.h"

using namespace GameEngine::Rendering;

// The scene benchmark reads the present mode from the swapchain Info log line.
// The Metal line uses the Vulkan names, so one parser reads both backends.
TEST(MetalPresentModeLogField, NamesTheDisplaySyncSettingAsAVulkanPresentMode)
{
    EXPECT_EQ(MetalMappings::PresentModeLogField(true), "presentMode=FIFO displayPaced=true");
    EXPECT_EQ(MetalMappings::PresentModeLogField(false), "presentMode=IMMEDIATE displayPaced=false");
}
