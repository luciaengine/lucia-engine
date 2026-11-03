#include "TileMapECS/TileMapPixelGrid.h"

#include "Components/Rendering/Camera.h"
#include "ECS/Entity.h" // defines ECS::World; ECS/World.h only forward-declares it
#include "ECS/World.h"
#include "Engine/Rendering/TileMapCameraPixelGrid.h"

#include "Components/TileMap/TileMapLayerRef.h"
#include "ECS/ECSTemplates.h"
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

// --- Pure derivation --------------------------------------------------------

TEST(TileMapPixelGrid, TilePixelsOverCellWidthIsTheDensity)
{
    // 16 px tile on a one-unit cell is the forest demo's 16 PPU; the same tile on
    // a two-unit cell is half the density, because density is art over world grid.
    EXPECT_FLOAT_EQ(DerivePixelsPerUnit(16, 1.0f), 16.0f);
    EXPECT_FLOAT_EQ(DerivePixelsPerUnit(16, 2.0f), 8.0f);
    EXPECT_FLOAT_EQ(DerivePixelsPerUnit(32, 1.0f), 32.0f);
    EXPECT_FLOAT_EQ(DerivePixelsPerUnit(8, 0.5f), 16.0f);
}

TEST(TileMapPixelGrid, DegenerateInputsResolveToUnknownRatherThanZeroOrInfinity)
{
    // A half-loaded tileset must read as "no answer" so the caller keeps the
    // authored fallback. Returning 0 or dividing by 0 would have the camera derive
    // world-units-per-texel from a density it can't invert.
    EXPECT_FLOAT_EQ(DerivePixelsPerUnit(0, 1.0f), kUnresolvedPixelsPerUnit);
    EXPECT_FLOAT_EQ(DerivePixelsPerUnit(-16, 1.0f), kUnresolvedPixelsPerUnit);
    EXPECT_FLOAT_EQ(DerivePixelsPerUnit(16, 0.0f), kUnresolvedPixelsPerUnit);
    EXPECT_FLOAT_EQ(DerivePixelsPerUnit(16, -1.0f), kUnresolvedPixelsPerUnit);
}

// --- Auto / manual resolution policy ----------------------------------------
//
// A bare world resolves no tilemap density, which is exactly the fallback case:
// auto must degrade to the authored value rather than to zero.

TEST(TileMapPixelGridResolution, EmptyWorldResolvesNoDensity)
{
    ECS::World world;
    EXPECT_FLOAT_EQ(Engine::Renderer::ResolveScenePixelsPerUnit(world), kUnresolvedPixelsPerUnit);
}

TEST(TileMapPixelGridResolution, AutoFallsBackToTheAuthoredValueWithoutATileMap)
{
    // Zero regression for a tilemap-less pixel-art scene: turning Auto on must not
    // change what the camera renders at when there is nothing to detect.
    ECS::World world;
    Components::Camera camera{};
    camera.PixelPerfect = true;
    camera.PixelPerfectAutoPixelsPerUnit = true;
    camera.PixelPerfectPixelsPerUnit = 24;

    EXPECT_EQ(Engine::Renderer::ResolvePixelPerfectPixelsPerUnit(world, camera), 24u);
}

TEST(TileMapPixelGridResolution, ManualReturnsTheAuthoredValueUntouched)
{
    ECS::World world;
    Components::Camera camera{};
    camera.PixelPerfect = true;
    camera.PixelPerfectAutoPixelsPerUnit = false;
    camera.PixelPerfectPixelsPerUnit = 32;

    EXPECT_EQ(Engine::Renderer::ResolvePixelPerfectPixelsPerUnit(world, camera), 32u);
}

TEST(TileMapPixelGridResolution, ResolvedDensityIsNeverZero)
{
    // World-units-per-texel is 1/density, so a zero would produce an infinity that
    // every downstream snap and extent calculation then inherits.
    ECS::World world;
    Components::Camera camera{};
    camera.PixelPerfect = true;
    camera.PixelPerfectPixelsPerUnit = 0;

    for (bool autoPpu : {true, false})
    {
        camera.PixelPerfectAutoPixelsPerUnit = autoPpu;
        EXPECT_GE(Engine::Renderer::ResolvePixelPerfectPixelsPerUnit(world, camera), 1u)
            << "auto " << autoPpu;
    }
}

TEST(TileMapPixelGridResolution, RecreatedStoreWithReusedGuidAndVersionUsesNewDensity)
{
    ASSERT_FALSE(TileMapRuntimeStore::IsInitialized());
    const GUID mapGuid = GUID::Generate();
    const GUID tileSetGuid = GUID::Generate();
    const auto directory = std::filesystem::temp_directory_path() /
        ("TileMapPixelGrid_" + mapGuid.ToString());
    std::filesystem::create_directories(directory);
    struct Cleanup
    {
        std::filesystem::path Directory;
        ~Cleanup()
        {
            if (TileMapRuntimeStore::IsInitialized()) TileMapRuntimeStore::Shutdown();
            std::error_code error;
            std::filesystem::remove_all(Directory, error);
        }
    } cleanup{directory};
    const auto mapPath = directory / "grid.tilemap";
    const auto tileSetPath = directory / "grid.tileset";
    const auto writeAsset = [](const auto& asset, const std::filesystem::path& path)
    {
        Vector<uint8> bytes;
        if (!asset.SaveToData(bytes)) return false;
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        output.close();
        return output.good();
    };
    TileMap::TileSetAsset tileSet(tileSetGuid, tileSetPath);
    tileSet.SetAtlasTexture(GUID::Generate(), 32, 32);
    ASSERT_TRUE(tileSet.SliceGrid(16, 16, 0, 0));
    ASSERT_TRUE(writeAsset(tileSet, tileSetPath));
    uint64 firstVersion = 0;
    // The final density also exceeds Windows LONG_MAX while fitting uint32.
    for (const auto [cellWidth, expectedDensity] :
         {std::pair{1.0f, 16u}, std::pair{2.0f, 8u},
          std::pair{16.0f / 3000000000.0f, 3000000000u}})
    {
        TileMap::TileMapAsset map(mapGuid, mapPath);
        map.Data().CellWidth = cellWidth;
        const uint32 layerId = map.Data().AddLayer("Ground", TileMap::TileMapLayerType::Tiles);
        map.Data().FindLayer(layerId)->TileSetGuid = tileSetGuid;
        ASSERT_TRUE(writeAsset(map, mapPath));
        TileMapRuntimeStore::Initialize();
        auto& store = TileMapRuntimeStore::Get();
        ASSERT_TRUE(store.LoadMapFromFile(mapGuid, mapPath));
        ASSERT_TRUE(store.LoadTileSetFromFile(tileSetGuid, tileSetPath));
        if (cellWidth == 1.0f) firstVersion = store.EditVersion(mapGuid);
        else EXPECT_EQ(store.EditVersion(mapGuid), firstVersion);
        ECS::World world;
        const auto entity = world.CreateEntity();
        Components::TileMapLayerRef layer{};
        layer.TileMap.Set(mapGuid);
        layer.LayerId = layerId;
        world.AddComponentImmediate(entity, layer);
        EXPECT_FLOAT_EQ(Engine::Renderer::ResolveScenePixelsPerUnit(world), 16.0f / cellWidth);
        Components::Camera camera{};
        camera.PixelPerfectAutoPixelsPerUnit = true;
        EXPECT_EQ(Engine::Renderer::ResolvePixelPerfectPixelsPerUnit(world, camera),
                  expectedDensity);
        TileMapRuntimeStore::Shutdown();
    }
}

TEST(TileMapPixelGrid, NonFiniteAndOverflowingDensityIsUnresolved)
{
    EXPECT_FLOAT_EQ(DerivePixelsPerUnit(16, std::numeric_limits<float>::infinity()), 0.0f);
    EXPECT_FLOAT_EQ(DerivePixelsPerUnit(16, std::numeric_limits<float>::quiet_NaN()), 0.0f);
    EXPECT_FLOAT_EQ(DerivePixelsPerUnit(16, std::numeric_limits<float>::denorm_min()), 0.0f);
}
