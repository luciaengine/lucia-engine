#include "TileMap/TileMapTerrainBrush.h"

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Core/Engine.h"
#include "TileMap/TileMapAsset.h"
#include "TileMap/TileSetAsset.h"
#include "TileMapECS/TileMapRuntimeStore.h"
#include "../TestTempDir.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <limits>

using namespace GameEngine;
using namespace GameEngine::Editor;
using namespace GameEngine::TileMap;
using namespace GameEngine::TileMapECS;

namespace
{
class TileMapTerrainBrushBoundaryTest : public testing::TestWithParam<int>
{
protected:
    void SetUp() override
    {
        auto& engine = EngineCore::GetInstance();
        m_OwnEngine = !engine.IsInitialized();
        if (m_OwnEngine)
        {
            ApplicationConfig config{};
            config.AssetDirectory = ".";
            config.WorkspaceDirectory = ".";
            config.EnableEditor = true;
            ASSERT_TRUE(engine.Initialize(config));
        }
        m_OwnStore = !TileMapRuntimeStore::IsInitialized();
        if (m_OwnStore)
            TileMapRuntimeStore::Initialize();
        m_Root = TestUtils::MakeUniqueTempDirectory("tile_brush_boundary");
        std::filesystem::create_directories(m_Root);
        m_SetGuid = GUID::Generate();
        m_MapGuid = GUID::Generate();
        TileSetAsset tiles(m_SetGuid, m_Root / "Ground.tileset");
        TerrainSetDef terrain;
        terrain.Name = "Ground";
        terrain.MatchMode = TerrainMatchMode::CornersAndSides;
        terrain.Terrains.push_back(TerrainDef{"Ground", 0xFFFFFFFFu});
        tiles.TerrainSets().push_back(terrain);
        // Every neighborhood has exactly one match. Tile id exposes the occupied slots.
        for (uint32 mask = 0; mask < 256; ++mask)
        {
            TileSetTile tile;
            tile.Id = mask + 1;
            tile.Peering.TerrainSet = 0;
            tile.Peering.Center = 0;
            for (uint32 slot = 0; slot < kPeeringSlotCount; ++slot)
                tile.Peering.Neighbors[slot] = (mask & (1u << slot)) ? 0 : -1;
            tiles.Tiles().push_back(tile);
        }
        Vector<uint8> bytes;
        ASSERT_TRUE(tiles.SaveToData(bytes));
        Write(m_Root / "Ground.tileset", bytes);
        AssetMetadata meta{};
        meta.Guid = m_SetGuid;
        meta.Path = m_Root / "Ground.tileset";
        meta.Name = "Ground";
        ASSERT_TRUE(engine.GetAssetManager().GetRegistry().RegisterAssetMetadata(meta));
        TileMapAsset map(m_MapGuid, m_Root / "Map.tilemap");
        m_Layer = map.Data().AddLayer("Ground", TileMapLayerType::Tiles);
        ASSERT_TRUE(map.SaveToData(bytes));
        Write(m_Root / "Map.tilemap", bytes);
        ASSERT_TRUE(TileMapRuntimeStore::Get().LoadMapFromFile(m_MapGuid, m_Root / "Map.tilemap"));
        ASSERT_TRUE(m_Brush.Begin(m_SetGuid, 0));
    }

    void TearDown() override
    {
        if (EngineCore::GetInstance().IsInitialized())
            EngineCore::GetInstance().GetAssetManager().GetRegistry().UnregisterAsset(m_SetGuid);
        if (m_OwnStore)
            TileMapRuntimeStore::Shutdown();
        if (m_OwnEngine)
            EngineCore::GetInstance().Shutdown();
        std::error_code ec;
        std::filesystem::remove_all(m_Root, ec);
    }

    static void Write(const std::filesystem::path& path, const Vector<uint8>& bytes)
    {
        std::ofstream file(path, std::ios::binary);
        file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        ASSERT_TRUE(file.good());
    }

    bool AlongX() const { return GetParam() < 2; }
    bool Maximum() const { return GetParam() % 2 == 0; }
    int32 Edge() const { return Maximum() ? std::numeric_limits<int32>::max() : std::numeric_limits<int32>::min(); }
    void Put(int32 coordinate)
    {
        ASSERT_TRUE(TileMapRuntimeStore::Get().SetCell(m_MapGuid, m_Layer,
            AlongX() ? coordinate : 0, AlongX() ? 0 : coordinate, MakeCell(1)));
    }
    std::vector<TerrainCellWrite> Paint(int32 coordinate, int16 terrain = 0)
    {
        std::vector<TerrainCellWrite> writes;
        m_Brush.Paint(m_MapGuid, m_Layer, AlongX() ? coordinate : 0,
                      AlongX() ? 0 : coordinate, terrain, writes);
        return writes;
    }

    TileMapTerrainBrush m_Brush;
    GUID m_SetGuid;
    GUID m_MapGuid;
    uint32 m_Layer = 0;
    std::filesystem::path m_Root;
    bool m_OwnEngine = false;
    bool m_OwnStore = false;
};

TEST_P(TileMapTerrainBrushBoundaryTest, EndpointDoesNotReadOrWriteTheOppositeEdge)
{
    Put(Maximum() ? std::numeric_limits<int32>::min() : std::numeric_limits<int32>::max());
    const auto writes = Paint(Edge());
    ASSERT_EQ(writes.size(), 1u);
    EXPECT_EQ(writes[0].CellX, AlongX() ? Edge() : 0);
    EXPECT_EQ(writes[0].CellY, AlongX() ? 0 : Edge());
    EXPECT_EQ(CellTileId(writes[0].Cell), 1u); // all eight neighbors are empty
    const auto erase = Paint(Edge(), -1);
    ASSERT_EQ(erase.size(), 1u);
    EXPECT_EQ(erase[0].Cell, kEmptyCell);
}

TEST_P(TileMapTerrainBrushBoundaryTest, OneInwardRepaintsTheEdgeWithoutReadingAcrossIt)
{
    Put(Edge());
    Put(Maximum() ? std::numeric_limits<int32>::min() : std::numeric_limits<int32>::max());
    const auto writes = Paint(Edge() + (Maximum() ? -1 : 1));
    ASSERT_EQ(writes.size(), 2u);
    const auto edge = std::find_if(writes.begin(), writes.end(), [this](const auto& write)
    {
        return write.CellX == (AlongX() ? Edge() : 0) && write.CellY == (AlongX() ? 0 : Edge());
    });
    ASSERT_NE(edge, writes.end());
    // The edge sees only the newly painted inward neighbor, never the opposite-edge sentinel.
    const uint32 inwardSlot = AlongX() ? (Maximum() ? 7u : 3u) : (Maximum() ? 5u : 1u);
    EXPECT_EQ(CellTileId(edge->Cell), (1u << inwardSlot) + 1u);
}

INSTANTIATE_TEST_SUITE_P(AllFourEdges, TileMapTerrainBrushBoundaryTest, testing::Range(0, 4));
} // namespace
