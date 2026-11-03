#include "TileMap/TileLayerEdits.h"
#include "TileMap/TileReplaceEdit.h"

#include "TileMap/TileCellEncoding.h"
#include "TileMap/TileMapAsset.h"
#include "TileMap/TileMapData.h"
#include "TileMapECS/TileMapRuntimeStore.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

using namespace GameEngine;
using GameEngine::TileMap::MakeCell;
using Editor::TileLayerEdits::LayerStateSnapshot;

namespace
{
constexpr const char* kMapGuidText = "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee";
constexpr uint32 kGroundLayerId = 1;
constexpr uint32 kDecorLayerId = 2;

/** Writes a two-layer map through the real serializer, so the fixture cannot drift. */
std::filesystem::path WriteFixture(const std::string& name)
{
    const GUID guid(kMapGuidText);
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / ("TileLayerEditsTests_" + name + ".tilemap");
    TileMap::TileMapAsset asset(guid, path);
    TileMap::TileMapData& data = asset.Data();
    const uint32 groundId = data.AddLayer("Ground", TileMap::TileMapLayerType::Tiles);
    TileMap::TileMapLayerData& ground = *data.FindLayer(groundId);
    ground.TileSetGuid = GUID("11111111-1111-1111-1111-111111111111");
    ground.SetCell(0, 0, MakeCell(7));
    ground.SetCell(1, 0, MakeCell(7, /*flipH*/ true));
    ground.SetCell(2, 0, MakeCell(8));
    // A second chunk holds the same tile, so the walk must cross chunks.
    ground.SetCell(40, 40, MakeCell(7, false, /*flipV*/ true));
    const uint32 decorId = data.AddLayer("Decor", TileMap::TileMapLayerType::Tiles);
    TileMap::TileMapLayerData& decor = *data.FindLayer(decorId);
    decor.TileSetGuid = ground.TileSetGuid;
    decor.SetCell(0, 0, MakeCell(7));
    data.AddLayer("Semantics", TileMap::TileMapLayerType::IntGrid);
    Vector<uint8> bytes;
    EXPECT_TRUE(asset.SaveToData(bytes));
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    out.close();
    return path;
}

class TileLayerEditsTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_FALSE(TileMapECS::TileMapRuntimeStore::IsInitialized());
        TileMapECS::TileMapRuntimeStore::Initialize();
        m_Guid = GUID(kMapGuidText);
        m_Path = WriteFixture(::testing::UnitTest::GetInstance()->current_test_info()->name());
        ASSERT_TRUE(Store().LoadMapFromFile(m_Guid, m_Path));
    }

    void TearDown() override
    {
        TileMapECS::TileMapRuntimeStore::Shutdown();
        std::error_code ec;
        std::filesystem::remove(m_Path, ec);
    }

    TileMapECS::TileMapRuntimeStore& Store() { return TileMapECS::TileMapRuntimeStore::Get(); }

    GUID m_Guid;
    std::filesystem::path m_Path;
};
} // namespace

TEST_F(TileLayerEditsTest, ReplaceTileRewritesEveryMatchKeepsFlipsAndUndoes)
{
    size_t cells = 0;
    std::unique_ptr<Editor::TileMapEditCommand> command =
        Editor::ReplaceTileEverywhere(Store(), m_Guid, {kGroundLayerId}, 7u, 9u, cells);
    ASSERT_NE(command, nullptr);
    EXPECT_EQ(cells, 3u);

    EXPECT_EQ(TileMap::CellTileId(Store().GetCell(m_Guid, kGroundLayerId, 0, 0)), 9u);
    const TileMap::TileCell flipped = Store().GetCell(m_Guid, kGroundLayerId, 1, 0);
    EXPECT_EQ(TileMap::CellTileId(flipped), 9u);
    EXPECT_TRUE(TileMap::CellFlipH(flipped));
    EXPECT_TRUE(TileMap::CellFlipV(Store().GetCell(m_Guid, kGroundLayerId, 40, 40)));
    // Tile 8 and the other layer are untouched.
    EXPECT_EQ(TileMap::CellTileId(Store().GetCell(m_Guid, kGroundLayerId, 2, 0)), 8u);
    EXPECT_EQ(TileMap::CellTileId(Store().GetCell(m_Guid, kDecorLayerId, 0, 0)), 7u);

    command->Undo();
    EXPECT_EQ(TileMap::CellTileId(Store().GetCell(m_Guid, kGroundLayerId, 0, 0)), 7u);
    EXPECT_TRUE(TileMap::CellFlipH(Store().GetCell(m_Guid, kGroundLayerId, 1, 0)));
    command->Do();
    EXPECT_EQ(TileMap::CellTileId(Store().GetCell(m_Guid, kGroundLayerId, 0, 0)), 9u);
}

TEST_F(TileLayerEditsTest, ReplaceTileWithZeroClearsAndSkipsLockedAndUnmatchedLayers)
{
    TileMap::TileMapData* data = Store().GetMap(m_Guid);
    ASSERT_NE(data, nullptr);
    data->FindLayer(kDecorLayerId)->Locked = true;

    size_t cells = 0;
    std::unique_ptr<Editor::TileMapEditCommand> command = Editor::ReplaceTileEverywhere(
        Store(), m_Guid, {kGroundLayerId, kDecorLayerId}, 7u, 0u, cells);
    ASSERT_NE(command, nullptr);
    EXPECT_EQ(cells, 3u) << "the locked layer's cell must not count";
    EXPECT_TRUE(TileMap::CellIsEmpty(Store().GetCell(m_Guid, kGroundLayerId, 0, 0)));
    EXPECT_EQ(TileMap::CellTileId(Store().GetCell(m_Guid, kDecorLayerId, 0, 0)), 7u);

    // Nothing to do: no command, no count.
    EXPECT_EQ(Editor::ReplaceTileEverywhere(Store(), m_Guid, {kGroundLayerId}, 123u, 1u, cells),
              nullptr);
    EXPECT_EQ(cells, 0u);
    EXPECT_EQ(Editor::ReplaceTileEverywhere(Store(), m_Guid, {kGroundLayerId}, 8u, 8u, cells),
              nullptr);
}

TEST_F(TileLayerEditsTest, SetLayersTileSetBindsTileLayersOnlyAndBumpsStructure)
{
    const GUID other("22222222-2222-2222-2222-222222222222");
    const uint64 before = Store().StructureVersion(m_Guid);
    EXPECT_TRUE(Editor::TileLayerEdits::SetLayersTileSet(Store(), m_Guid,
                                                          {kGroundLayerId, kDecorLayerId, 3u}, other));
    const TileMap::TileMapData* data = Store().GetMap(m_Guid);
    EXPECT_EQ(data->FindLayer(kGroundLayerId)->TileSetGuid, other);
    EXPECT_EQ(data->FindLayer(kDecorLayerId)->TileSetGuid, other);
    EXPECT_TRUE(data->FindLayer(3u)->TileSetGuid.IsNull()) << "an IntGrid layer has no tileset";
    EXPECT_GT(Store().StructureVersion(m_Guid), before);

    // Already bound: no change reported, no version moved.
    const uint64 after = Store().StructureVersion(m_Guid);
    EXPECT_FALSE(Editor::TileLayerEdits::SetLayersTileSet(Store(), m_Guid, {kGroundLayerId}, other));
    EXPECT_EQ(Store().StructureVersion(m_Guid), after);
    EXPECT_FALSE(Editor::TileLayerEdits::SetLayersTileSet(Store(), m_Guid, {kGroundLayerId}, GUID{}));
}

TEST(TileLayerSnapshotTests, DiffersFromSeesEveryEditableField)
{
    LayerStateSnapshot a;
    a.Entries.push_back({1u, "Ground", false, GUID{}, true, 0.0f, true});
    LayerStateSnapshot b = a;
    EXPECT_FALSE(b.DiffersFrom(a));
    b.Entries[0].TileSetGuid = GUID("33333333-3333-3333-3333-333333333333");
    EXPECT_TRUE(b.DiffersFrom(a));
    b = a;
    b.Entries[0].ZOffset = 0.5f;
    EXPECT_TRUE(b.DiffersFrom(a));
    b = a;
    b.Entries.clear();
    EXPECT_TRUE(b.DiffersFrom(a)) << "a different layer count is a difference";
}
