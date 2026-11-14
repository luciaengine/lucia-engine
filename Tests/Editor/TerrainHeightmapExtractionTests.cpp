#include "Components/Terrain/Terrain.h"
#include "Components/Transform.h"
#include "Core/Engine.h"
#include "ECS/World.h"
#include "Engine/Rendering/RenderServices.h"
#include "SplineECS/SplineService.h"
#include "Terrain/Heightfield.h"
#include "TerrainECS/Systems/TerrainExtractionSystem.h"
#include "TerrainECS/TerrainRenderFeature.h"
#include "TerrainECS/TerrainService.h"

#include "TestDeviceHelper.h"

#include <gtest/gtest.h>

TEST(TerrainHeightmapExtraction, KeepsTheAuthoredBaseAndAStableTiledHandle)
{
    namespace Components = GameEngine::Components;
    namespace TerrainECS = GameEngine::TerrainECS;
    auto& engine = GameEngine::EngineCore::GetInstance();
    if (!engine.IsInitialized())
    {
        GameEngine::ApplicationConfig config{};
        config.AssetDirectory = ".";
        config.WorkspaceDirectory = ".";
        config.EnableEditor = true;
        ASSERT_TRUE(engine.Initialize(config));
    }

    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    {
        GameEngine::Engine::Renderer::RenderServices renderServices;
        ASSERT_TRUE(renderServices.Initialize(device.get()));
        auto& feature = renderServices.EnsureFeature<TerrainECS::TerrainRenderFeature>();
        ASSERT_TRUE(feature.Initialize(device.get()));
        if (!TerrainECS::TerrainService::IsInitialized())
            TerrainECS::TerrainService::Initialize();
        if (!GameEngine::SplineECS::SplineService::IsInitialized())
            GameEngine::SplineECS::SplineService::Initialize();
        auto& service = TerrainECS::TerrainService::Get();
        const GameEngine::GUID heightmap = GameEngine::GUID::Generate();
        service.SeedDecodedHeightmapForTests(heightmap, GameEngine::Terrain::HeightfieldData(2, 2, 0.5f));

        GameEngine::ECS::World world;
        auto entity = world.Create();
        entity.Set(Components::WorldTransform{});
        Components::Terrain authored{};
        authored.SizeX = 2048.0f;
        authored.SizeZ = 1024.0f;
        authored.HeightScale = 100.0f;
        authored.SamplesPerMeter = 1.0f;
        authored.BaseSource = Components::TerrainBaseSource::HeightmapAsset;
        authored.TerrainAssetGuid.Set(heightmap);
        entity.Set(authored);
        world.ProcessCommands();
        TerrainECS::TerrainExtractionSystem extraction(&renderServices);
        constexpr float kFrameSeconds = 1.0f / 60.0f;
        extraction.Update(world, kFrameSeconds);
        const auto* first = world.GetComponent<Components::Terrain>(entity.GetHandle());
        ASSERT_NE(first, nullptr);
        const TerrainECS::TiledTerrainHandle firstHandle{first->TiledTerrainHandle, first->TiledTerrainGeneration};
        const auto* tiled = service.GetTiledTerrainData(firstHandle);
        EXPECT_NE(tiled, nullptr);
        if (tiled)
        {
            EXPECT_EQ(tiled->Config.Base.Source, Components::TerrainBaseSource::HeightmapAsset);
            EXPECT_EQ(tiled->Config.Base.HeightmapGuid, heightmap);
            EXPECT_NE(tiled->Config.Base.Heightmap, nullptr);
            EXPECT_FALSE(service.CompareTiledTerrainConfig(tiled->Config, authored).BaseChanged);
        }

        extraction.Update(world, kFrameSeconds);
        const auto* second = world.GetComponent<Components::Terrain>(entity.GetHandle());
        const TerrainECS::TiledTerrainHandle secondHandle{second->TiledTerrainHandle, second->TiledTerrainGeneration};
        EXPECT_EQ(secondHandle.Index, firstHandle.Index);
        EXPECT_EQ(secondHandle.Generation, firstHandle.Generation);
        if (const auto* current = service.GetTiledTerrainData(secondHandle))
            feature.ReleaseTerrainResources(
                TerrainECS::TerrainHandle{current->GlobalGpuHandleIndex, current->GlobalGpuHandleGeneration});
        service.DestroyTiledTerrain(secondHandle);
        service.EvictDecodedHeightmap(heightmap);
        renderServices.Shutdown();
    }
    device->Shutdown();
}
