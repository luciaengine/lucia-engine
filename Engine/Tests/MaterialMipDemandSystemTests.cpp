// The demand system of material mip streaming, on the path every project runs: streaming off.

#include "ECSModules/Rendering/Systems/MaterialMipDemandSystem.h"

#include "Engine/Rendering/RenderServices.h"

#include <gtest/gtest.h>

using namespace GameEngine;
using GameEngine::Engine::Renderer::MaterialMipDemandSystem;

TEST(MaterialMipDemandSystem, IsDisabledWithoutRenderServices)
{
    const MaterialMipDemandSystem system(nullptr);

    EXPECT_FALSE(system.IsEnabled());
}

TEST(MaterialMipDemandSystem, IsDisabledWhileTheRenderServicesHaveNoScene)
{
    Engine::Renderer::RenderServices renderServices;
    const MaterialMipDemandSystem system(&renderServices);

    EXPECT_FALSE(system.IsEnabled());
}
