// The feature policy RendererProfile derives from a device's capabilities, for the flags that
// select shader code: what a device without the capability runs instead is decided here.

#include <gtest/gtest.h>

#include "Rendering/Core/Device.h"
#include "Rendering/Core/RendererProfile.h"

#include <cstdlib>
#include <string>

using namespace GameEngine::Rendering;

namespace
{
RenderingDeviceCapabilities DesktopCapabilities()
{
    RenderingDeviceCapabilities caps{};
    caps.supportsBindlessResources = true;
    caps.supportsBufferDeviceAddress = true;
    caps.supportsShaderInterpolationFunctions = true;
    caps.supportsShaderClipDistance = true;
    return caps;
}

// Sets GE_FORCE_COMPAT for a scope and restores what the process had.
struct ScopedForceCompat
{
    ScopedForceCompat()
    {
        const char* previous = std::getenv("GE_FORCE_COMPAT");
        Had = previous != nullptr;
        Previous = Had ? previous : "";
        Set("1");
    }
    ~ScopedForceCompat() { Set(Had ? Previous.c_str() : ""); }
    static void Set(const char* value)
    {
#if defined(_WIN32)
        _putenv_s("GE_FORCE_COMPAT", value);
#else
        if (value[0] == '\0')
            unsetenv("GE_FORCE_COMPAT");
        else
            setenv("GE_FORCE_COMPAT", value, 1);
#endif
    }
    bool Had = false;
    std::string Previous;
};
} // namespace

TEST(RendererProfile, KnownConstrainedMemoryReducesImpostorTilesAndResidentBytes)
{
    RenderingDeviceCapabilities caps = DesktopCapabilities();
    const auto unknown = RendererProfile::FromCapabilities(caps);
    caps.dedicatedVideoMemory = 8ull << 30;
    const auto fullMemory = RendererProfile::FromCapabilities(caps);
    caps.dedicatedVideoMemory = 4ull << 30;
    const auto compact = RendererProfile::FromCapabilities(caps);

    EXPECT_EQ(unknown.ImpostorSwitchPixels, 128u);
    EXPECT_EQ(fullMemory.ImpostorSwitchPixels, 128u);
    EXPECT_EQ(compact.ImpostorSwitchPixels, 96u);
    EXPECT_EQ(unknown.ImpostorFramePixels, fullMemory.ImpostorFramePixels);
    EXPECT_EQ(unknown.ImpostorAtlasBudgetBytes, fullMemory.ImpostorAtlasBudgetBytes);
    EXPECT_EQ(compact.ImpostorFramePixels * 2, fullMemory.ImpostorFramePixels);
    EXPECT_EQ(compact.ImpostorAtlasBudgetBytes * 4, fullMemory.ImpostorAtlasBudgetBytes);
    EXPECT_TRUE(compact.UseGpuDrivenDraws);
}

TEST(RendererProfile, CompatibilityUsesCompactImpostorMemoryWithoutAnEstimate)
{
    RenderingDeviceCapabilities caps = DesktopCapabilities();
    caps.supportsBindlessResources = false;
    const auto profile = RendererProfile::FromCapabilities(caps);
    EXPECT_TRUE(profile.IsCompat());
    EXPECT_EQ(profile.ImpostorSwitchPixels, 96u);
    EXPECT_EQ(profile.ImpostorFramePixels, 64u);
    EXPECT_EQ(profile.ImpostorAtlasBudgetBytes, 64ull << 20);
}

TEST(RendererProfile, InterpolationFunctionsFollowEnabledShaderCapability)
{
    RenderingDeviceCapabilities caps = DesktopCapabilities();
    EXPECT_TRUE(RendererProfile::FromCapabilities(caps).UseInterpolationFunctions);
    caps.supportsShaderInterpolationFunctions = false;
    EXPECT_FALSE(RendererProfile::FromCapabilities(caps).UseInterpolationFunctions)
        << "a device without enabled interpolation functions must keep the centroid footprint";
}

TEST(RendererProfile, ForcedCompatibilityTakesNoInterpolationFunctions)
{
    RenderingDeviceCapabilities caps = DesktopCapabilities();
    ScopedForceCompat forced;
    ASSERT_TRUE(ApplyForceCompatOverride(caps));
    EXPECT_FALSE(caps.supportsShaderInterpolationFunctions);
    EXPECT_FALSE(RendererProfile::FromCapabilities(caps).UseInterpolationFunctions);
}

TEST(RendererProfile, ForcedCompatibilityTakesNoMeshWorkgroups)
{
    RenderingDeviceCapabilities caps = DesktopCapabilities();
    caps.supportsMeshShaders = true;
    caps.supportsIndirectMeshTasks = true;
    ScopedForceCompat forced;
    ASSERT_TRUE(ApplyForceCompatOverride(caps));
    EXPECT_FALSE(caps.supportsMeshShaders);
    EXPECT_FALSE(caps.supportsIndirectMeshTasks);
}

TEST(RendererProfile, ForcedCompatibilityTakesNoShaderFloat64)
{
    RenderingDeviceCapabilities caps = DesktopCapabilities();
    caps.supportsShaderFloat64 = true;
    ScopedForceCompat forced;
    ASSERT_TRUE(ApplyForceCompatOverride(caps));
    EXPECT_FALSE(caps.supportsShaderFloat64) << "WGSL has no 64-bit float type";
}

TEST(RendererProfile, ClipDistanceFollowsShaderClipDistance)
{
    RenderingDeviceCapabilities caps = DesktopCapabilities();
    EXPECT_TRUE(RendererProfile::FromCapabilities(caps).UseClipDistance);
    caps.supportsShaderClipDistance = false;
    EXPECT_FALSE(RendererProfile::FromCapabilities(caps).UseClipDistance)
        << "a device without shaderClipDistance must not be handed gl_ClipDistance";

    caps = DesktopCapabilities();
    ScopedForceCompat forced;
    ASSERT_TRUE(ApplyForceCompatOverride(caps));
    EXPECT_FALSE(RendererProfile::FromCapabilities(caps).UseClipDistance) << "WGSL takes no clip distance";
}
