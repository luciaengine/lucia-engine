#include <gtest/gtest.h>

#include "Engine/Rendering/ShadowMapRenderFeature.h"
#include "ScopedCompatShaderProfile.h"

using GameEngine::TestSupport::ScopedCompatShaderProfile;
using GameEngine::Engine::Renderer::ShadowFilterQuality;
using GameEngine::Engine::Renderer::ShadowMapRenderFeature;

// A desktop device starts with PCSS; the browser's compat shader profile starts with DPCF, the same
// contact hardening without PCSS's blocker search.
TEST(ShadowFilterDefault, TheCompatProfileStartsWithDpcf)
{
    {
        const ShadowMapRenderFeature desktop;
        EXPECT_EQ(desktop.GetFilterQuality(), ShadowFilterQuality::PCSS);
    }
    ScopedCompatShaderProfile compat;
    const ShadowMapRenderFeature web;
    EXPECT_EQ(web.GetFilterQuality(), ShadowFilterQuality::DPCF);
}
