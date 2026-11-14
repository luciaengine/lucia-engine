#include "Source/Vulkan/VulkanDevice/VulkanDevice.h"
#include "Source/Vulkan/VulkanFaultInjection.h"
#include "Tests/ScopedEnvVar.h"
#include "TestTempDir.h"

#include <gtest/gtest.h>
#include <GLFW/glfw3.h>

#include <memory>

using namespace GameEngine::Rendering;
using GameEngine::Rendering::Tests::ScopedEnvVar;

TEST(SwapchainAcquireInjection, ParsesBothPresentAndAcquireOnlyModes)
{
    auto both = ParseFaultInjection(nullptr, nullptr, nullptr, "2");
    EXPECT_TRUE(both.Active());
    EXPECT_TRUE(both.acquireSuboptimalAlsoPresent);
    EXPECT_FALSE(both.ShouldForceAcquireSuboptimal(1));
    EXPECT_TRUE(both.ShouldForceAcquireSuboptimal(2));
    EXPECT_FALSE(both.ShouldForceAcquireSuboptimal(3));
    auto acquireOnly = ParseFaultInjection(nullptr, nullptr, nullptr, "2:acquire");
    EXPECT_FALSE(acquireOnly.acquireSuboptimalAlsoPresent);
    EXPECT_TRUE(acquireOnly.ShouldForceAcquireSuboptimal(4));
    for (const char* invalid : {"", "0", "garbage", "2:other"})
        EXPECT_FALSE(ParseFaultInjection(nullptr, nullptr, nullptr, invalid).Active()) << invalid;
}

// Real WSI acquisition: injection only changes a successful driver's result,
// preserving its valid image index and signaled binary semaphore. Present()
// submits the normal transition/relay even though this fixture draws no scene.
class SwapchainAcquire : public ::testing::TestWithParam<const char*>
{
  protected:
    void SetUp() override
    {
        m_Injection = std::make_unique<ScopedEnvVar>("GE_VK_FORCE_ACQUIRE_SUBOPTIMAL", GetParam());
        ASSERT_EQ(glfwInit(), GLFW_TRUE);
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        m_Window = glfwCreateWindow(320, 200, "SwapchainAcquire", nullptr, nullptr);
        ASSERT_NE(m_Window, nullptr);
        DeviceDesc desc{};
        desc.preferredAPI = GraphicsAPI::Vulkan;
        desc.enableDebugLayer = true;
        desc.pipelineCacheDirectory = m_Cache.Path().string();
        m_Device = DeviceFactory::CreateDevice(desc);
        if (!m_Device || !m_Device->Initialize(desc))
            GTEST_SKIP() << "No Vulkan device available";
        ASSERT_TRUE(m_Device->CreateAndActivateWindowTarget(m_Window, 320, 200, &m_Target));
    }

    void TearDown() override
    {
        if (m_Device)
        {
            m_Device->WaitForIdle();
            const auto stats = m_Device->GetValidationStats();
            RecordProperty("validation_layer_active", stats.Enabled);
            EXPECT_EQ(stats.ErrorCount, 0u);
            m_Device->Shutdown();
            m_Device.reset();
        }
        if (m_OtherWindow) glfwDestroyWindow(m_OtherWindow);
        if (m_Window) glfwDestroyWindow(m_Window);
        glfwTerminate();
    }

    GameEngine::TestUtils::ScopedTempDir m_Cache{
        GameEngine::TestUtils::MakeUniqueTempDirectory("swapchain-acquire")};
    ScopedEnvVar m_NoLost{"GE_VK_FORCE_DEVICE_LOST", "0"};
    ScopedEnvVar m_NoTimeout{"GE_VK_FORCE_BEGINFRAME_TIMEOUT", "0"};
    ScopedEnvVar m_NoHang{"GE_VK_FORCE_GPU_HANG", "0"};
    std::unique_ptr<ScopedEnvVar> m_Injection;
    GLFWwindow* m_Window = nullptr;
    GLFWwindow* m_OtherWindow = nullptr;
    std::unique_ptr<IDevice> m_Device;
    WindowTargetHandle m_Target{};
};

TEST_P(SwapchainAcquire, SuboptimalImageIsPresentedBeforeRecreation)
{
    // Nine frames reuse all frame slots. Every second acquire is suboptimal;
    // the successful presents between them must not retain stale recreate state.
    for (int frame = 1; frame <= 9; ++frame)
    {
        ASSERT_TRUE(m_Device->BeginFrame()) << "acquire " << frame;
        m_Device->Present();
        auto* vk = dynamic_cast<VulkanDevice*>(m_Device.get());
        ASSERT_NE(vk, nullptr);
        if (frame % 2 == 0)
            EXPECT_EQ(vk->GetCurrentSwapchainImageIndex(), UINT32_MAX) << "present " << frame;
        else
            EXPECT_LT(vk->GetCurrentSwapchainImageIndex(), vk->GetSwapchainImageCount()) << "present " << frame;
    }
}

TEST_P(SwapchainAcquire, RecreatingOneTargetPreservesTheOtherTargetsProgress)
{
    ASSERT_TRUE(m_Device->BeginFrame());
    m_Device->Present();
    const auto progressA = m_Device->GetFrameIndexForWindowTarget(m_Target);
    // Owned by the fixture: both windows outlive device shutdown on failures too.
    m_OtherWindow = glfwCreateWindow(256, 160, "SwapchainAcquireOther", nullptr, nullptr);
    ASSERT_NE(m_OtherWindow, nullptr);
    WindowTargetHandle targetB{};
    ASSERT_TRUE(m_Device->CreateAndActivateWindowTarget(m_OtherWindow, 256, 160, &targetB));
    ASSERT_TRUE(m_Device->BeginFrame()); // injected acquire #2 on B
    m_Device->Present();
    EXPECT_EQ(m_Device->GetFrameIndexForWindowTarget(m_Target), progressA);
    ASSERT_TRUE(m_Device->SetActiveWindowTarget(m_Target));
    ASSERT_TRUE(m_Device->BeginFrame()); // normal acquire #3 on A
    m_Device->Present();
    EXPECT_NE(m_Device->GetFrameIndexForWindowTarget(m_Target), progressA);
    m_Device->WaitForIdle();
    ASSERT_TRUE(m_Device->DestroyWindowTarget(targetB));
}

INSTANTIATE_TEST_SUITE_P(DriverResults, SwapchainAcquire,
    ::testing::Values("2", "2:acquire"));
