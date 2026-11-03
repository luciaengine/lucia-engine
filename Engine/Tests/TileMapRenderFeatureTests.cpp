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
    Vector<uint8> mapBytes;
    ASSERT_TRUE(map.SaveToData(mapBytes));
    std::ofstream(mapPath, std::ios::binary).write(reinterpret_cast<const char*>(mapBytes.data()), mapBytes.size());
    ASSERT_TRUE(TileMapRuntimeStore::Get().LoadMapFromFile(mapGuid, mapPath));
    TileMapLayerDraw draw;
    draw.Map = mapGuid;
    draw.LayerId = layerId;
    Feature->SetLayerDraws(12, {draw});
    float32 viewProj[16]{};
    for (int i = 0; i < 16; i += 5) viewProj[i] = 1.0f;
    const float32 position[] = {0, 0, 0};
    const float32 forward[] = {0, 0, 1};
    std::vector<TileMapChunkDraw> batches;
    Feature->BuildViewDraws(Services, viewProj, position, forward, 1.0f, batches, 12);
    ASSERT_EQ(batches.size(), 1u);
    ASSERT_EQ(batches[0].InstanceCount, 1u);
    ASSERT_TRUE(batches[0].Atlas.IsValid());
    ASSERT_TRUE(batches[0].Instances.IsValid());
    Feature->BuildViewDraws(Services, viewProj, position, forward, 1.0f, batches, 13);
    EXPECT_TRUE(batches.empty());
}
