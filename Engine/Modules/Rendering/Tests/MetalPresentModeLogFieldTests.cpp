#include <gtest/gtest.h>

#include "../Source/Metal/MetalMappings.h"

using namespace GameEngine::Rendering;

// A timing window reads the present mode back from the swapchain Info log line.
// The Metal line uses the Vulkan names, so one parser reads both backends.
TEST(MetalPresentModeLogField, NamesTheDisplaySyncSettingAsAVulkanPresentMode)
{
    EXPECT_EQ(MetalMappings::PresentModeLogField(true), "presentMode=FIFO displayPaced=true");
    EXPECT_EQ(MetalMappings::PresentModeLogField(false), "presentMode=IMMEDIATE displayPaced=false");
}
