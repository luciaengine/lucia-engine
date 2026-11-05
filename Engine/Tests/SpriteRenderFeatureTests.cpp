#include "Engine/Rendering/2D/SpriteRenderFeature.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/2D/Systems/SpriteAnimationSystem.h"
#include "Assets/AssetManager.h"
#include "Core/Application.h"
#include "Core/Engine.h"
#include "EngineTestShaderSetup.h"
#include "Rendering/Common/Utils.h"
#include "Mathematics/Vector3.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

using namespace GameEngine::Engine::Renderer;

TEST(SpriteRenderFeature, SnapshotOnlyBelongsToItsExtractedWorld)
{
    RenderServices services;
    SpriteRenderFeature feature(services);
    std::vector<SpriteDraw> draws(1);
    feature.SetSpriteDraws(12, draws);
    EXPECT_TRUE(feature.HasSpriteDraws(12));
    EXPECT_TRUE(feature.HasSpriteDraws(0));
    EXPECT_FALSE(feature.HasSpriteDraws(13));
    draws.resize(1);
    feature.SetSpriteDraws(13, draws);
    EXPECT_TRUE(feature.HasSpriteDraws(13));
    EXPECT_FALSE(feature.HasSpriteDraws(12));
    draws.clear();
    feature.SetSpriteDraws(13, draws);
    EXPECT_FALSE(feature.HasSpriteDraws(13));
}

TEST(SpriteRenderFeature, SystemRegistersFeatureBeforeExtractionRuns)
{
    RenderServices services;
    SpriteAnimationSystem system(&services);
    EXPECT_NE(services.GetFeature<SpriteRenderFeature>(), nullptr);
}

namespace
{
class SpriteWorldDraw : public testing::Test
{
  protected:
    const std::filesystem::path OriginalDirectory = std::filesystem::current_path();
    std::filesystem::path Workspace;
    std::unique_ptr<GameEngine::Rendering::IDevice> Device;
    RenderServices Services;
    std::unique_ptr<SpriteRenderFeature> Feature;

    void TearDown() override
    {
        Feature.reset();
        Services.Shutdown();
        if (Device) Device->Shutdown();
        GameEngine::EngineCore::GetInstance().Shutdown();
        GameEngine::Rendering::Utils::SetShaderFileLoader(nullptr);
        GameEngine::Rendering::Utils::SetShaderPathResolver(
            &GameEngine::Testing::EngineTestShaderPathResolver);
        // Engine initialization can change the process directory. Restore it
        // before removing the generated project, including after fatal asserts.
        std::error_code error;
        std::filesystem::current_path(OriginalDirectory, error);
        EXPECT_FALSE(error) << error.message();
        if (error)
            return;
        std::filesystem::remove_all(Workspace, error);
    }
};
}

TEST_F(SpriteWorldDraw, MatchingWorldProducesGpuBatchBeforeForeignWorldIsRejected)
{
    using namespace GameEngine;
    Workspace = std::filesystem::temp_directory_path() / ("ge-sprite-world-" + GUID::Generate().ToString());
    std::filesystem::create_directories(Workspace / "Assets");
    Rendering::DeviceDesc deviceDesc{};
    deviceDesc.preferredAPI = Rendering::GraphicsAPI::Vulkan;
    deviceDesc.enableDebugLayer = false;
    deviceDesc.enableDynamicRendering = true;
    deviceDesc.pipelineCacheDirectory = (Workspace / "PipelineCache").string();
    Device = Rendering::DeviceFactory::CreateDevice(deviceDesc);
    ASSERT_NE(Device, nullptr);
    ASSERT_TRUE(Device->Initialize(deviceDesc));
    auto& engine = EngineCore::GetInstance();
    ScriptsConfig scripts;
    scripts.disableClr = true;
    scripts.enableHotReload = false;
    scripts.enableAsyncHotReload = false;
    scripts.enableAutoProjectGeneration = false;
    engine.SetScriptsConfig(scripts);
    ApplicationConfig config;
    config.WorkspaceDirectory = Workspace.string();
    config.EnableEditor = false;
    ASSERT_TRUE(engine.Initialize(config));
    // Engine initialization installs the project resolver. GPU test shaders
    // belong to the staged build, not the generated asset-only project.
    Rendering::Utils::SetShaderFileLoader(nullptr);
    Rendering::Utils::SetShaderPathResolver(&Testing::EngineTestShaderPathResolver);
    ASSERT_TRUE(Services.Initialize(Device.get()));
    Feature = std::make_unique<SpriteRenderFeature>(Services);
    ASSERT_TRUE(Feature->Initialize(Device.get()));

    // SVG loads synchronously through the production texture service, avoiding
    // a cook worker race while still requiring an actual uploaded GPU texture.
    const auto texturePath = Workspace / "Assets" / "sprite.svg";
    std::ofstream(texturePath) << R"(<svg xmlns="http://www.w3.org/2000/svg" width="4" height="4"><rect width="4" height="4" fill="white"/></svg>)";
    const auto registered = engine.GetAssetManager().GetRegistry().RegisterAssetByPath(texturePath);
    ASSERT_TRUE(registered.IsOk());
    const auto cameraId = Services.Views().AllocateCamera("SpriteWorldCamera");
    Rendering::CameraData camera{};
    for (int i = 0; i < 16; i += 5)
        camera.view[i] = camera.proj[i] = camera.viewProj[i] = 1.0f;
    Services.Views().SetCameraData(cameraId, camera);
    const auto viewId = Services.Views().AllocateView("SpriteWorldView", cameraId);
    Services.Views().SetViewWorldId(viewId, 12);
    Services.Views().SetViewRenderLayerMask(viewId, 0xFFFFFFFFu);
    std::vector<SpriteDraw> draws(1);
    draws[0].Texture = registered.Value();
    draws[0].RenderLayers = 0xFFFFFFFFu;
    draws[0].AxisX[0] = draws[0].AxisY[1] = 0.5f;
    Feature->SetSpriteDraws(12, draws);
    std::vector<SpriteBatch> batches;
    Feature->BuildViewDraws(Services, viewId, batches);
    ASSERT_EQ(batches.size(), 1u);
    ASSERT_EQ(batches[0].InstanceCount, 1u);
    ASSERT_TRUE(batches[0].Atlas.IsValid());
    ASSERT_TRUE(batches[0].Instances.IsValid());
    Services.Views().SetViewWorldId(viewId, 13);
    Feature->BuildViewDraws(Services, viewId, batches);
    EXPECT_TRUE(batches.empty());

    Services.Views().SetViewWorldId(viewId, 12);
    SpriteDraw sprite;
    sprite.Texture = registered.Value();
    sprite.Center[2] = 0.5f;
    sprite.AxisX[0] = sprite.AxisY[1] = 0.25f;
    const std::vector<Mathematics::Vector3> outside = {
        {-3, 0, 0.5f}, {3, 0, 0.5f}, {0, -3, 0.5f},
        {0, 3, 0.5f}, {0, 0, -1}, {0, 0, 2}};
    for (const auto& center : outside)
    {
        SCOPED_TRACE(::testing::Message() << center.x << "," << center.y << "," << center.z);
        std::copy_n(center.v, 3, sprite.Center);
        draws = {sprite};
        Feature->SetSpriteDraws(12, draws);
        Feature->BuildViewDraws(Services, viewId, batches);
        EXPECT_TRUE(batches.empty());
    }

    // A mirrored, skewed quad can cross the viewport even with its center outside.
    sprite.Center[0] = 1.5f;
    sprite.Center[1] = 0;
    sprite.Center[2] = 0.5f;
    sprite.AxisX[0] = -1.0f;
    sprite.AxisX[1] = 0.3f;
    sprite.AxisY[0] = 0.4f;
    draws = {sprite};
    Feature->SetSpriteDraws(12, draws);
    Feature->BuildViewDraws(Services, viewId, batches);
    ASSERT_EQ(batches.size(), 1u);
    EXPECT_EQ(batches[0].InstanceCount, 1u);

    // Visibility is per view: moving the camera can reveal the same snapshot.
    sprite.Center[0] = 4.0f;
    sprite.AxisX[0] = sprite.AxisY[1] = 0.25f;
    sprite.AxisX[1] = sprite.AxisY[0] = 0;
    draws = {sprite};
    Feature->SetSpriteDraws(12, draws);
    Feature->BuildViewDraws(Services, viewId, batches);
    EXPECT_TRUE(batches.empty());
    camera.viewProj[12] = -4.0f;
    Services.Views().SetCameraData(cameraId, camera);
    Feature->BuildViewDraws(Services, viewId, batches);
    ASSERT_EQ(batches.size(), 1u);
    EXPECT_EQ(batches[0].InstanceCount, 1u);

    camera.viewProj[12] = 0;
    Services.Views().SetCameraData(cameraId, camera);
    sprite.Center[0] = 1.26f;
    draws = {sprite};
    Feature->SetSpriteDraws(12, draws);
    Feature->BuildViewDraws(Services, viewId, batches);
    EXPECT_TRUE(batches.empty());
    PixelPerfectViewState pixelPerfect;
    pixelPerfect.Active = true;
    pixelPerfect.WorldUnitsPerTexel = 0.2f;
    Services.Views().SetViewPixelPerfect(viewId, pixelPerfect);
    Feature->BuildViewDraws(Services, viewId, batches);
    EXPECT_EQ(batches.size(), 1u) << "Pixel snapping moves the left edge onto the viewport";

    Services.Views().SetViewRenderLayerMask(viewId, 0u);
    Feature->BuildViewDraws(Services, viewId, batches);
    EXPECT_TRUE(batches.empty());
}
