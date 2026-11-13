#include "TileMapECS/TileMapRenderFeature.h"
#include "TileMapECS/TileMapRuntimeStore.h"
#include "TileMap/TileMapAsset.h"
#include "TileMap/TileSetAsset.h"
#include "TileMap/TileCellEncoding.h"
#include "Engine/Rendering/RenderServices.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"
#include "Rendering/Common/Utils.h"
#include "EngineTestShaderSetup.h"
#include "Assets/AssetManager.h"
#include "Core/Application.h"
#include "Core/Engine.h"
#include "Components/Rendering/RenderSorting.h"
#include "Engine/Rendering/RenderOrigin.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Engine/Rendering/RenderSorting.h"
#include "Engine/Rendering/SortingLayers.h"
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

using namespace GameEngine::Engine::Renderer;
using namespace GameEngine::TileMapECS;

namespace
{
class TileWorldDraw : public testing::Test
{
  protected:
    std::filesystem::path OriginalDirectory = std::filesystem::current_path();
    std::filesystem::path Workspace;
    std::unique_ptr<GameEngine::Rendering::IDevice> Device;
    RenderServices Services;
    std::unique_ptr<TileMapRenderFeature> Feature;

    void TearDown() override
    {
        Feature.reset();
        Services.Shutdown();
        if (Device) Device->Shutdown();
        GameEngine::EngineCore::GetInstance().Shutdown();
        GameEngine::Rendering::Utils::SetShaderFileLoader(nullptr);
        GameEngine::Rendering::Utils::SetShaderPathResolver(&GameEngine::Testing::EngineTestShaderPathResolver);
        std::error_code error;
        std::filesystem::current_path(OriginalDirectory, error);
        EXPECT_FALSE(error) << error.message();
        if (error) return;
        std::filesystem::remove_all(Workspace, error);
    }
};
}

TEST_F(TileWorldDraw, MatchingWorldProducesGpuBatchBeforeForeignWorldIsRejected)
{
    using namespace GameEngine;
    Workspace = std::filesystem::temp_directory_path() / ("ge-tile-world-" + GUID::Generate().ToString());
    std::filesystem::create_directories(Workspace / "Assets");
    Rendering::DeviceDesc deviceDesc{};
    deviceDesc.preferredAPI = Rendering::GraphicsAPI::Vulkan;
    deviceDesc.enableDebugLayer = false;
    deviceDesc.enableSwapchain = false;
    deviceDesc.enableDynamicRendering = true;
    deviceDesc.pipelineCacheDirectory = (Workspace / "PipelineCache").string();
    Device = Rendering::DeviceFactory::CreateDevice(deviceDesc);
    ASSERT_TRUE(Device && Device->Initialize(deviceDesc)) << "Vulkan device required for GPU batch regression";

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
    TileMapRuntimeStore::Initialize();
    Rendering::Utils::SetShaderFileLoader(nullptr);
    Rendering::Utils::SetShaderPathResolver(&Testing::EngineTestShaderPathResolver);
    ASSERT_TRUE(Services.Initialize(Device.get()));
    Feature = std::make_unique<TileMapRenderFeature>(Services);
    Rendering::ShaderPackage shader;
    std::string shaderError;
    ASSERT_TRUE(Rendering::LoadShaderPkg("Shaders/tilemap.shaderpkg", Device->PreferredShaderSource(), shader, &shaderError))
        << shaderError;
    ASSERT_TRUE(Feature->Initialize(Device.get()));

    // SVG loads synchronously through the production texture service, avoiding
    // a cook worker race while still requiring an actual uploaded GPU texture.
    const auto texturePath = Workspace / "Assets" / "sprite.svg";
    std::ofstream(texturePath) << R"(<svg xmlns="http://www.w3.org/2000/svg" width="4" height="4"><rect width="4" height="4" fill="white"/></svg>)";
    const auto registered = engine.GetAssetManager().GetRegistry().RegisterAssetByPath(texturePath);
    ASSERT_TRUE(registered.IsOk());
    const auto setPath = Workspace / "Assets" / "world.tileset";
    TileMap::TileSetAsset set(GUID::Generate(), setPath);
    set.SetAtlasTexture(registered.Value(), 4, 4);
    ASSERT_TRUE(set.SliceGrid(4, 4, 0, 0));
    Vector<uint8> setBytes;
    ASSERT_TRUE(set.SaveToData(setBytes));
    std::ofstream(setPath, std::ios::binary).write(reinterpret_cast<const char*>(setBytes.data()), setBytes.size());
    const auto setRegistration = engine.GetAssetManager().GetRegistry().RegisterAssetByPath(setPath);
    ASSERT_TRUE(setRegistration.IsOk());

    const auto mapPath = Workspace / "Assets" / "world.tilemap";
    const auto mapGuid = GUID::Generate();
    TileMap::TileMapAsset map(mapGuid, mapPath);
    const auto layerId = map.Data().AddLayer("Ground", TileMap::TileMapLayerType::Tiles);
    auto* layer = map.Data().FindLayer(layerId);
    ASSERT_NE(layer, nullptr);
    layer->TileSetGuid = setRegistration.Value();
    layer->SetCell(0, 0, TileMap::MakeCell(1));
    layer->SetCell(1, 0, TileMap::MakeCell(1));
    layer->SetCell(0, 2, TileMap::MakeCell(1));
    layer->SetCell(1000000, -1000000, TileMap::MakeCell(1));
    Vector<uint8> mapBytes;
    ASSERT_TRUE(map.SaveToData(mapBytes));
    std::ofstream(mapPath, std::ios::binary).write(reinterpret_cast<const char*>(mapBytes.data()), mapBytes.size());
    ASSERT_TRUE(TileMapRuntimeStore::Get().LoadMapFromFile(mapGuid, mapPath));
    TileMapLayerDraw draw;
    draw.Map = mapGuid;
    draw.LayerId = layerId;
    Feature->SetLayerDraws(12, {draw});
    Rendering::CameraData camera{};
    camera.view = camera.proj = camera.viewProj = Mathematics::Matrix4x4{};
    SetCameraEye(camera, Mathematics::WorldPosition{});
    Mathematics::WorldPosition position{};
    const float32 forward[] = {0, 0, 1};
    std::vector<TileMapChunkDraw> batches;
    Feature->BuildViewDraws(Services, camera, 1.0f, batches, 12);
    ASSERT_EQ(batches.size(), 1u);
    ASSERT_EQ(batches[0].InstanceCount, 3u);
    ASSERT_TRUE(batches[0].Atlas.IsValid());
    ASSERT_TRUE(batches[0].Instances.IsValid());
    Feature->BuildViewDraws(Services, camera, 1.0f, batches, 13);
    EXPECT_TRUE(batches.empty());

    draw.Sorting.SortByY = true;
    Feature->SetLayerDraws(12, {draw});
    Feature->BuildViewDraws(Services, camera, 1.0f, batches, 12);
    ASSERT_EQ(batches.size(), 2u) << "Y-sorted cells in one chunk must allow a sprite between rows";
    EXPECT_EQ(batches[0].InstanceCount, 2u);
    EXPECT_EQ(batches[0].FirstInstance, 0u);
    EXPECT_EQ(batches[1].InstanceCount, 1u);
    EXPECT_EQ(batches[1].FirstInstance, 2u);
    EXPECT_EQ(batches[0].Sort.Depth, 0.0f);
    EXPECT_EQ(batches[1].Sort.Depth, 2.0f);
    RenderSortKey character;
    character.Depth = 1.0f;
    EXPECT_TRUE(RenderSortBefore(batches[1].Sort, character));
    EXPECT_TRUE(RenderSortBefore(character, batches[0].Sort));

    draw.Basis[1].y = -2.0f;
    camera.proj[0][0] = camera.proj[1][1] = 0.01f;
    camera.viewProj = camera.proj * camera.view;
    SetCameraEye(camera, Mathematics::WorldPosition{});
    draw.Position.Y = -3.0f;
    draw.Sorting.SortOriginY = 0.25f;
    draw.Sorting.SortY = draw.Position.Y + draw.Basis[1].y * draw.Sorting.SortOriginY;
    Feature->SetLayerDraws(12, {draw});
    Feature->BuildViewDraws(Services, camera, 1.0f, batches, 12);
    ASSERT_EQ(batches.size(), 2u);
    EXPECT_FLOAT_EQ(batches[0].Sort.Depth, -3.5f);
    EXPECT_FLOAT_EQ(batches[1].Sort.Depth, -7.5f);

    draw.Basis[0].y = 1.0f;
    Feature->SetLayerDraws(12, {draw});
    Feature->BuildViewDraws(Services, camera, 1.0f, batches, 12);
    ASSERT_EQ(batches.size(), 3u) << "A tilted tile row has distinct world ground points";
    EXPECT_FLOAT_EQ(batches[0].Sort.Depth, -3.0f);
    EXPECT_FLOAT_EQ(batches[1].Sort.Depth, -2.0f);
    EXPECT_FLOAT_EQ(batches[2].Sort.Depth, -7.0f);

    ECS::World world;
    Components::RenderSorting sorting;
    sorting.SortByY = true;
    Components::WorldTransform tileTransform;
    tileTransform.Position.Y = 1024.0;
    auto tileEntity = world.Create(sorting, tileTransform);
    Components::WorldTransform actorTransform;
    actorTransform.Position.Y = 1025.0;
    auto actorEntity = world.Create(sorting, actorTransform);
    world.ProcessCommands();
    const auto layers = Rendering::SortingLayers::Defaults();
    draw = TileMapLayerDraw{};
    draw.Map = mapGuid;
    draw.LayerId = layerId;
    draw.Position = tileTransform.Position;
    position = Mathematics::WorldPosition{0.0, 1024.0, 0.0};
    camera.view = Mathematics::Matrix4x4::Translation({0.0f, -1024.0f, 0.0f});
    camera.viewProj = camera.proj * camera.view;
    SetCameraEye(camera, position);
    draw.Sorting = ResolveRenderSorting(world, tileEntity.GetHandle(), layers);
    Feature->SetLayerDraws(12, {draw});
    Feature->BuildViewDraws(Services, camera, 1.0f, batches, 12);
    ASSERT_EQ(batches.size(), 2u);
    const auto actorKey = MakeRenderSortKey(ResolveRenderSorting(world, actorEntity.GetHandle(), layers),
                                           0, position, forward);
    EXPECT_TRUE(RenderSortBefore(batches[1].Sort, actorKey));
    EXPECT_TRUE(RenderSortBefore(actorKey, batches[0].Sort));

    draw.Position = Mathematics::WorldPosition{-999999.75, -99999999.75, 0.5};
    draw.Basis[0].y = 0.25f;
    draw.Basis[1].y = -2.0f;
    draw.WorldZ = 0.5;
    draw.ParallaxX = 0.0f;
    draw.Sorting.SortOriginY = 0.25f;
    draw.Sorting.SortY = draw.Position.Y - 0.5;
    position = Mathematics::WorldPosition{100000000.125, -97749999.75, 0.0};
    camera.view = Mathematics::Matrix4x4::Translation({-static_cast<float32>(position.X), -static_cast<float32>(position.Y), 0.0f});
    camera.viewProj = camera.proj * camera.view;
    SetCameraEye(camera, position);
    TileMapPreview preview;
    preview.Map = mapGuid;
    preview.LayerId = layerId;
    preview.OriginX = 1000001;
    preview.OriginY = -1000000;
    preview.Instances = {{0u, TileMap::MakeCell(1)}};
    Feature->SetPreview(std::move(preview));
    Feature->SetLayerDraws(12, {draw});
    Feature->BuildViewDraws(Services, camera, 1.0f, batches, 12);
    ASSERT_EQ(batches.size(), 2u);
    const double originX = static_cast<double>(camera.renderOriginSector[0]) * Mathematics::kWorldSectorSize;
    const double originY = static_cast<double>(camera.renderOriginSector[1]) * Mathematics::kWorldSectorSize;
    EXPECT_FLOAT_EQ(batches[0].Push.Model[12], static_cast<float32>(100000000.375 - originX));
    EXPECT_FLOAT_EQ(batches[0].Push.Model[13], static_cast<float32>(-97749999.75 - originY));
    EXPECT_DOUBLE_EQ(batches[0].Sort.Depth, -97750000.125);
    EXPECT_FLOAT_EQ(batches[1].Push.Model[12], static_cast<float32>(100000001.375 - originX));
    EXPECT_FLOAT_EQ(batches[1].Push.Model[13], static_cast<float32>(-97749999.5 - originY));
    EXPECT_DOUBLE_EQ(batches[1].Sort.Depth, -97749999.875);
}
