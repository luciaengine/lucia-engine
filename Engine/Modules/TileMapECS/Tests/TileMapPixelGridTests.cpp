#include "TileMapECS/TileMapPixelGrid.h"

#include "Components/Rendering/Camera.h"
#include "Components/TileMap/TileMapLayerRef.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "Engine/Rendering/Camera.h"
#include "Engine/Rendering/CameraAspectRatio.h"
#include "TileMap/TileMapAsset.h"
#include "TileMap/TileSetAsset.h"
#include "TileMapECS/TileMapRuntimeStore.h"

#include <filesystem>
#include <fstream>
#include <limits>
#include <utility>

#include <gtest/gtest.h>

using namespace GameEngine;
using namespace GameEngine::TileMapECS;
using Engine::Renderer::ResolvePixelPerfectPixelsPerUnit;

namespace
{

template <typename Asset>
bool WriteAsset(const Asset& asset, const std::filesystem::path& path)
{
    Vector<uint8> bytes;
    if (!asset.SaveToData(bytes))
        return false;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    output.close();
    return output.good();
}

// A runtime store that holds one tile map with one layer of 16 px tiles.
class TileMapPixelGridSceneTests : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_FALSE(TileMapRuntimeStore::IsInitialized());
        m_Directory = std::filesystem::temp_directory_path() / ("TileMapPixelGrid_" + m_MapGuid.ToString());
        std::filesystem::create_directories(m_Directory);
        TileMap::TileSetAsset tileSet(m_TileSetGuid, m_Directory / "grid.tileset");
        tileSet.SetAtlasTexture(GUID::Generate(), 32, 32);
        ASSERT_TRUE(tileSet.SliceGrid(16, 16, 0, 0));
        ASSERT_TRUE(WriteAsset(tileSet, m_Directory / "grid.tileset"));
    }

    void TearDown() override
    {
        if (TileMapRuntimeStore::IsInitialized())
            TileMapRuntimeStore::Shutdown();
        std::error_code error;
        std::filesystem::remove_all(m_Directory, error);
    }

    // Writes the map with `cellWidth`, loads it into a fresh store and returns the layer id.
    uint32 LoadMap(float32 cellWidth)
    {
        if (TileMapRuntimeStore::IsInitialized())
            TileMapRuntimeStore::Shutdown();
        const std::filesystem::path mapPath = m_Directory / "grid.tilemap";
        TileMap::TileMapAsset map(m_MapGuid, mapPath);
        map.Data().CellWidth = cellWidth;
        const uint32 layerId = map.Data().AddLayer("Ground", TileMap::TileMapLayerType::Tiles);
        map.Data().FindLayer(layerId)->TileSetGuid = m_TileSetGuid;
        EXPECT_TRUE(WriteAsset(map, mapPath));
        TileMapRuntimeStore::Initialize();
        EXPECT_TRUE(TileMapRuntimeStore::Get().LoadMapFromFile(m_MapGuid, mapPath));
        EXPECT_TRUE(TileMapRuntimeStore::Get().LoadTileSetFromFile(m_TileSetGuid, m_Directory / "grid.tileset"));
        return layerId;
    }

    ECS::EntityHandle AddLayer(ECS::World& world, uint32 layerId)
    {
        const ECS::EntityHandle entity = world.CreateEntity();
        Components::TileMapLayerRef layer{};
        layer.TileMap.Set(m_MapGuid);
        layer.LayerId = layerId;
        world.AddComponentImmediate(entity, layer);
        return entity;
    }

    GUID m_MapGuid = GUID::Generate();
    GUID m_TileSetGuid = GUID::Generate();
    std::filesystem::path m_Directory;
};

Components::Camera PixelPerfectCamera(bool autoPixelsPerUnit, uint32 authored)
{
    Components::Camera camera{};
    camera.Perspective = false;
    camera.PixelPerfect = true;
    camera.PixelPerfectAutoPixelsPerUnit = autoPixelsPerUnit;
    camera.PixelPerfectPixelsPerUnit = authored;
    return camera;
}

} // namespace

TEST(TileMapPixelGrid, TilePixelsOverCellWidthIsTheDensity)
{
    EXPECT_FLOAT_EQ(DerivePixelsPerUnit(16, 1.0f), 16.0f);
    EXPECT_FLOAT_EQ(DerivePixelsPerUnit(16, 2.0f), 8.0f);
    EXPECT_FLOAT_EQ(DerivePixelsPerUnit(32, 1.0f), 32.0f);
    EXPECT_FLOAT_EQ(DerivePixelsPerUnit(8, 0.5f), 16.0f);
}

TEST(TileMapPixelGrid, DegenerateInputsAreUnresolved)
{
    EXPECT_FLOAT_EQ(DerivePixelsPerUnit(0, 1.0f), kUnresolvedPixelsPerUnit);
    EXPECT_FLOAT_EQ(DerivePixelsPerUnit(-16, 1.0f), kUnresolvedPixelsPerUnit);
    EXPECT_FLOAT_EQ(DerivePixelsPerUnit(16, 0.0f), kUnresolvedPixelsPerUnit);
    EXPECT_FLOAT_EQ(DerivePixelsPerUnit(16, -1.0f), kUnresolvedPixelsPerUnit);
    EXPECT_FLOAT_EQ(DerivePixelsPerUnit(16, std::numeric_limits<float>::infinity()), kUnresolvedPixelsPerUnit);
    EXPECT_FLOAT_EQ(DerivePixelsPerUnit(16, std::numeric_limits<float>::quiet_NaN()), kUnresolvedPixelsPerUnit);
    EXPECT_FLOAT_EQ(DerivePixelsPerUnit(16, std::numeric_limits<float>::denorm_min()), kUnresolvedPixelsPerUnit);
}

TEST(TileMapPixelGrid, WorldWithoutTileMapsUsesTheAuthoredValue)
{
    ECS::World world;
    EXPECT_FLOAT_EQ(ResolveScenePixelsPerUnit(world), kUnresolvedPixelsPerUnit);
    EXPECT_EQ(ResolvePixelPerfectPixelsPerUnit(world, PixelPerfectCamera(true, 24)), 24u);
    EXPECT_EQ(ResolvePixelPerfectPixelsPerUnit(world, PixelPerfectCamera(false, 32)), 32u);
    for (bool autoPixelsPerUnit : {true, false})
        EXPECT_EQ(ResolvePixelPerfectPixelsPerUnit(world, PixelPerfectCamera(autoPixelsPerUnit, 0)), 1u)
            << "auto " << autoPixelsPerUnit;
}

TEST_F(TileMapPixelGridSceneTests, AutoTakesTheTileSetDensityAndManualKeepsTheAuthoredValue)
{
    ECS::World world;
    AddLayer(world, LoadMap(1.0f));
    EXPECT_FLOAT_EQ(ResolveScenePixelsPerUnit(world), 16.0f);
    EXPECT_EQ(ResolvePixelPerfectPixelsPerUnit(world, PixelPerfectCamera(true, 32)), 16u);
    EXPECT_EQ(ResolvePixelPerfectPixelsPerUnit(world, PixelPerfectCamera(false, 32)), 32u);
}

TEST_F(TileMapPixelGridSceneTests, HiddenOrDisabledLayersSupplyNoDensity)
{
    ECS::World world;
    const uint32 layerId = LoadMap(1.0f);
    const ECS::EntityHandle hidden = AddLayer(world, layerId);
    Components::TileMapLayerRef hiddenLayer = *world.GetComponent<Components::TileMapLayerRef>(hidden);
    hiddenLayer.Visible = false;
    world.AddComponentImmediate(hidden, hiddenLayer);
    const ECS::EntityHandle disabled = AddLayer(world, layerId);
    world.AddComponentImmediate(disabled, ECS::Disabled{});
    EXPECT_FLOAT_EQ(ResolveScenePixelsPerUnit(world), kUnresolvedPixelsPerUnit);
}

TEST_F(TileMapPixelGridSceneTests, DensityFollowsTheCellWidthAndRoundsToTheNearestInteger)
{
    // The last density is above the Windows long range but fits uint32.
    for (const auto [cellWidth, expected] :
         {std::pair{1.0f, 16u}, std::pair{2.0f, 8u}, std::pair{1.0000001f, 16u},
          std::pair{16.0f / 3000000000.0f, 3000000000u}})
    {
        ECS::World world;
        AddLayer(world, LoadMap(cellWidth));
        EXPECT_EQ(ResolvePixelPerfectPixelsPerUnit(world, PixelPerfectCamera(true, 32)), expected)
            << "cell width " << cellWidth;
    }
}

TEST_F(TileMapPixelGridSceneTests, ActiveCameraSnapshotRendersAtTheResolvedDensity)
{
    ECS::World world;
    AddLayer(world, LoadMap(1.0f));
    const ECS::EntityHandle cameraEntity = world.CreateEntity();
    world.AddComponentImmediate(cameraEntity, Components::Transform{});
    world.AddComponentImmediate(cameraEntity, PixelPerfectCamera(true, 32));

    const auto active = Engine::Renderer::FindActiveCamera(world);
    ASSERT_TRUE(active.has_value());
    EXPECT_EQ(active->params.PixelPerfectPixelsPerUnit, 16u);
    EXPECT_EQ(world.GetComponent<Components::Camera>(cameraEntity)->PixelPerfectPixelsPerUnit, 32u)
        << "the authored value stays in the component";
    EXPECT_FLOAT_EQ(Engine::Renderer::ComputePixelPerfectViewState(active->params, 1280, 720).WorldUnitsPerTexel,
                    1.0f / 16.0f);

    world.AddComponentImmediate(cameraEntity, PixelPerfectCamera(false, 32));
    EXPECT_EQ(Engine::Renderer::FindActiveCamera(world)->params.PixelPerfectPixelsPerUnit, 32u);
}
