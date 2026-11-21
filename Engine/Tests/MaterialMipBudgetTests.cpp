// The default budget of material mip streaming: a share of the memory that the device reports.

#include "Engine/Rendering/MaterialMipStreaming/MaterialMipBudget.h"

#include "../Modules/Rendering/Tests/ScopedEnvVar.h"
#include "Rendering/Core/Device.h"

#include <gtest/gtest.h>

using namespace GameEngine;
using GameEngine::Engine::Renderer::MaterialMipBudget;

namespace
{
constexpr uint64 kGiB = 1024ull * 1024ull * 1024ull;
} // namespace

TEST(MaterialMipBudget, ADiscreteGpuGivesOneHalfOfItsDeviceLocalMemory)
{
    Rendering::RenderingDeviceCapabilities caps;
    caps.memoryTopology = Rendering::DeviceMemoryTopology{false, 256ull * 1024ull * 1024ull, 8 * kGiB};

    EXPECT_EQ(MaterialMipBudget::FromDevice(caps), 4 * kGiB);
}

TEST(MaterialMipBudget, UnifiedMemoryGivesOneQuarter)
{
    Rendering::RenderingDeviceCapabilities caps;
    caps.memoryTopology = Rendering::DeviceMemoryTopology{true, 32 * kGiB, 32 * kGiB};

    EXPECT_EQ(MaterialMipBudget::FromDevice(caps), 8 * kGiB);
}

TEST(MaterialMipBudget, ABackendWithNoTopologyUsesItsDedicatedVideoMemory)
{
    Rendering::RenderingDeviceCapabilities caps;
    caps.dedicatedVideoMemory = static_cast<size_t>(6 * kGiB);

    EXPECT_EQ(MaterialMipBudget::FromDevice(caps), 3 * kGiB);
}

TEST(MaterialMipBudget, ABackendThatReportsNoMemoryStillStreams)
{
    const Rendering::RenderingDeviceCapabilities caps;

    EXPECT_EQ(MaterialMipBudget::FromDevice(caps), MaterialMipBudget::kMinimumBytes);

    Rendering::RenderingDeviceCapabilities emptyTopology;
    emptyTopology.memoryTopology = Rendering::DeviceMemoryTopology{};
    EXPECT_EQ(MaterialMipBudget::FromDevice(emptyTopology), MaterialMipBudget::kMinimumBytes);
}

TEST(MaterialMipBudget, UnifiedMemoryWithASmallDeviceLocalHeapGetsTheMinimum)
{
    Rendering::RenderingDeviceCapabilities caps;
    caps.memoryTopology = Rendering::DeviceMemoryTopology{true, kGiB / 2, kGiB / 2};

    EXPECT_EQ(MaterialMipBudget::FromDevice(caps), MaterialMipBudget::kMinimumBytes);
}

#if !defined(NDEBUG) && !defined(PLATFORM_STEAMDECK)
TEST(MaterialMipBudget, ABuildWithNoNdebugReadsTheTestOverride)
{
    Rendering::RenderingDeviceCapabilities caps;
    caps.memoryTopology = Rendering::DeviceMemoryTopology{false, 0, 8 * kGiB};
    {
        const Rendering::Tests::ScopedEnvVar budget("GE_MATERIAL_MIP_BUDGET_MB", "3");
        EXPECT_EQ(MaterialMipBudget::ForPlatform(caps), 3ull * 1024ull * 1024ull);
    }
    {
        const Rendering::Tests::ScopedEnvVar off("GE_MATERIAL_MIP_BUDGET_MB", "0");
        EXPECT_EQ(MaterialMipBudget::ForPlatform(caps), 0u);
    }
    {
        const Rendering::Tests::ScopedEnvVar invalid("GE_MATERIAL_MIP_BUDGET_MB", "many");
        EXPECT_EQ(MaterialMipBudget::ForPlatform(caps), 4 * kGiB) << "a value that does not parse changed the budget";
    }
}
#endif
