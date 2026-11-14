#include "Engine/Rendering/2D/SpriteRenderFeature.h"
#include "Engine/Rendering/RenderOrigin.h"
#include "Components/Rendering/Light.h"
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
    camera.view = camera.proj = camera.viewProj = Mathematics::Matrix4x4{};
    const Rendering::CameraRanges cameraRanges{0.0f, 1.0f};
    SetCameraEye(camera, Mathematics::WorldPosition{});
    Services.Views().SetCameraData(cameraId, camera, cameraRanges);
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
    sprite.Center.Z = 0.5f;
    sprite.AxisX[0] = sprite.AxisY[1] = 0.25f;
    const std::vector<Mathematics::Vector3> outside = {
        {-3, 0, 0.5f}, {3, 0, 0.5f}, {0, -3, 0.5f},
        {0, 3, 0.5f}, {0, 0, -1}, {0, 0, 2}};
    for (const auto& center : outside)
    {
        SCOPED_TRACE(::testing::Message() << center.x << "," << center.y << "," << center.z);
        sprite.Center = Mathematics::WorldPosition::FromVector3(center);
        draws = {sprite};
        Feature->SetSpriteDraws(12, draws);
        Feature->BuildViewDraws(Services, viewId, batches);
        EXPECT_TRUE(batches.empty());
    }

    // A mirrored, skewed quad can cross the viewport even with its center outside.
    sprite.Center.X = 1.5f;
    sprite.Center.Y = 0;
    sprite.Center.Z = 0.5f;
    sprite.AxisX[0] = -1.0f;
    sprite.AxisX[1] = 0.3f;
    sprite.AxisY[0] = 0.4f;
    draws = {sprite};
    Feature->SetSpriteDraws(12, draws);
    Feature->BuildViewDraws(Services, viewId, batches);
    ASSERT_EQ(batches.size(), 1u);
    EXPECT_EQ(batches[0].InstanceCount, 1u);

    // Visibility is per view: moving the camera can reveal the same snapshot.
    sprite.Center.X = 4.0f;
    sprite.AxisX[0] = sprite.AxisY[1] = 0.25f;
    sprite.AxisX[1] = sprite.AxisY[0] = 0;
    draws = {sprite};
    Feature->SetSpriteDraws(12, draws);
    Feature->BuildViewDraws(Services, viewId, batches);
    EXPECT_TRUE(batches.empty());
    camera.view = Mathematics::Matrix4x4::Translation({-4.0f, 0.0f, 0.0f});
    camera.viewProj = camera.proj * camera.view;
    SetCameraEye(camera, Mathematics::WorldPosition{4.0, 0.0, 0.0});
    Services.Views().SetCameraData(cameraId, camera, cameraRanges);
    Feature->BuildViewDraws(Services, viewId, batches);
    ASSERT_EQ(batches.size(), 1u);
    EXPECT_EQ(batches[0].InstanceCount, 1u);

    camera.view = Mathematics::Matrix4x4{};
    camera.viewProj = camera.proj * camera.view;
    SetCameraEye(camera, Mathematics::WorldPosition{});
    Services.Views().SetCameraData(cameraId, camera, cameraRanges);
    sprite.Center.X = 1.26f;
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

    pixelPerfect.Active = false;
    Services.Views().SetViewPixelPerfect(viewId, pixelPerfect);
    constexpr double kBoundary = 100000256.0;
    sprite.Center = Mathematics::WorldPosition{kBoundary + 0.25, 0.0, 0.5};
    std::vector<SpriteLightDraw> lightDraws(1);
    lightDraws[0].Position = Mathematics::WorldPosition{kBoundary + 0.5, 0.0, 0.5};
    lightDraws[0].Gpu.PositionType[3] = static_cast<float32>(Components::LightType::Point);
    Feature->SetSpriteLights(lightDraws);
    const auto secondCameraId = Services.Views().AllocateCamera("SpriteOtherOrigin");
    const auto secondViewId = Services.Views().AllocateView("SpriteOtherOrigin", secondCameraId);
    Services.Views().SetViewWorldId(secondViewId, 12);
    Services.Views().SetViewRenderLayerMask(secondViewId, 0xFFFFFFFFu);
    Rendering::BufferHandle firstViewInstances;
    Rendering::BufferHandle firstViewLights;
    for (int viewIndex = 0; viewIndex < 2; ++viewIndex)
    {
        const double eyeX = kBoundary + (viewIndex == 0 ? -0.125 : 0.125);
        const auto targetCamera = viewIndex == 0 ? cameraId : secondCameraId;
        const auto targetView = viewIndex == 0 ? viewId : secondViewId;
        camera.view = Mathematics::Matrix4x4::Translation({-static_cast<float32>(eyeX), 0.0f, 0.0f});
        camera.viewProj = camera.proj * camera.view;
        SetCameraEye(camera, Mathematics::WorldPosition{eyeX, 0.0, 0.0});
        Services.Views().SetCameraData(targetCamera, camera, cameraRanges);
        draws = {sprite};
        Feature->SetSpriteDraws(12, draws);
        Feature->BuildViewDraws(Services, targetView, batches);
        ASSERT_EQ(batches.size(), 1u);
        const auto* instance = static_cast<const float32*>(Device->MapBuffer(batches[0].Instances));
        ASSERT_NE(instance, nullptr);
        EXPECT_FLOAT_EQ(instance[0], viewIndex == 0 ? 512.25f : -511.75f);
        Device->UnmapBuffer(batches[0].Instances);
        const auto* light = static_cast<const SpriteLight*>(Device->MapBuffer(batches[0].Lights));
        ASSERT_NE(light, nullptr);
        EXPECT_FLOAT_EQ(light[0].PositionType[0], viewIndex == 0 ? 512.5f : -511.5f);
        Device->UnmapBuffer(batches[0].Lights);
        if (viewIndex == 0)
        {
            firstViewInstances = batches[0].Instances;
            firstViewLights = batches[0].Lights;
        }
        else
        {
            EXPECT_NE(firstViewInstances, batches[0].Instances);
            EXPECT_NE(firstViewLights, batches[0].Lights);
        }
    }
    const auto* firstView = static_cast<const float32*>(Device->MapBuffer(firstViewInstances));
    ASSERT_NE(firstView, nullptr);
    EXPECT_FLOAT_EQ(firstView[0], 512.25f);
    Device->UnmapBuffer(firstViewInstances);
    const auto* firstLight = static_cast<const SpriteLight*>(Device->MapBuffer(firstViewLights));
    ASSERT_NE(firstLight, nullptr);
    EXPECT_FLOAT_EQ(firstLight[0].PositionType[0], 512.5f);
    Device->UnmapBuffer(firstViewLights);

    Services.Views().SetViewRenderLayerMask(viewId, 0u);
    Feature->BuildViewDraws(Services, viewId, batches);
    EXPECT_TRUE(batches.empty());
}
