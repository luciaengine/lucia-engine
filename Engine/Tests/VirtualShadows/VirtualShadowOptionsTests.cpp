#include "Engine/Rendering/VirtualShadows/VirtualShadowOptions.h"
#include "Engine/Rendering/VirtualShadows/VirtualShadowRenderFeature.h"

#include <gtest/gtest.h>

namespace GameEngine::Engine::Renderer
{
TEST(VirtualShadowOptions, AcceptsIndependentDirectionalAndLocalAddressGrids)
{
    VirtualShadowOptions options;
    options.DirectionalResolution = 16384;
    options.LocalResolution = 128;
    EXPECT_TRUE(options.IsValid());
    options.DirectionalResolution = 128;
    options.LocalResolution = 16384;
    EXPECT_TRUE(options.IsValid());
}

TEST(VirtualShadowOptions, RefusingAnInvalidConfigurationPreservesTheActiveSettings)
{
    VirtualShadowRenderFeature feature;
    VirtualShadowOptions options;
    options.Enabled = true;
    options.DirectionalResolution = 4096;
    options.LocalResolution = 1024;
    ASSERT_TRUE(feature.Configure(options));
    auto invalid = options;
    invalid.LocalResolution = 129;
    EXPECT_FALSE(feature.Configure(invalid));
    EXPECT_EQ(feature.GetOptions(), options);
}

TEST(VirtualShadowOptions, ThePoolHoldsFrom64To4096PhysicalPages)
{
    VirtualShadowOptions options;
    EXPECT_EQ(options.PhysicalPages, 2048u);
    for (const uint32_t pages : {64u, 4096u})
    {
        options.PhysicalPages = pages;
        EXPECT_TRUE(options.IsValid()) << pages;
    }
    for (const uint32_t pages : {0u, 63u, 4097u})
    {
        options.PhysicalPages = pages;
        EXPECT_FALSE(options.IsValid()) << pages;
    }
}

TEST(VirtualShadowOptions, RejectsPartialPagesAndUnsupportedAddressGrids)
{
    for (const uint32_t resolution : {0u, 64u, 768u, 32768u})
    {
        VirtualShadowOptions directional;
        directional.DirectionalResolution = resolution;
        EXPECT_FALSE(directional.IsValid());
        VirtualShadowOptions local;
        local.LocalResolution = resolution;
        EXPECT_FALSE(local.IsValid());
    }
}
} // namespace GameEngine::Engine::Renderer
