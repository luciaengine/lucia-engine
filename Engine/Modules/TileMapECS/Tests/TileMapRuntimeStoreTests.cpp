#include "TileMap/TileCellEncoding.h"
#include "TileMap/TileMapAsset.h"
#include "TileMap/TileRules.h"
#include "TileMapECS/TileMapRuntimeStore.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::TileMapECS;
using GameEngine::TileMap::MakeCell;

namespace
{

constexpr const char* kMapGuidText = "11111111-2222-3333-4444-555555555555";
constexpr uint32 kGroundLayerId = 1;

/**
 * Writes a two-layer .tilemap through the real serializer, so the fixture can
 * never drift from the format the store decodes.
 */
std::filesystem::path WriteFixture(const std::string& name)
{
    const GUID guid(kMapGuidText);
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / ("TileMapRuntimeStoreTests_" + name + ".tilemap");

    TileMap::TileMapAsset asset(guid, path);
    TileMap::TileMapData& data = asset.Data();
    data.CellWidth = 2.0f;
    data.CellHeight = 3.0f;

    const uint32 groundId = data.AddLayer("Ground", TileMap::TileMapLayerType::Tiles);
    TileMap::TileMapLayerData& ground = *data.FindLayer(groundId);
    ground.SetCell(0, 0, MakeCell(1));
    ground.SetCell(3, 2, MakeCell(2, /*flipH*/ true));
    // A second chunk, so per-chunk dirty tracking has something to distinguish.
    ground.SetCell(40, 40, MakeCell(3));
    // A negative-coordinate chunk exercises the floor-division chunk math.
    ground.SetCell(-1, -1, MakeCell(4));

    data.AddLayer("Decor", TileMap::TileMapLayerType::Tiles);

    Vector<uint8> bytes;
    EXPECT_TRUE(asset.SaveToData(bytes));
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    out.close();
    return path;
}

class TileMapRuntimeStoreTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_FALSE(TileMapRuntimeStore::IsInitialized());
        TileMapRuntimeStore::Initialize();
        m_Guid = GUID(kMapGuidText);
        m_Path = WriteFixture(::testing::UnitTest::GetInstance()->current_test_info()->name());
        ASSERT_TRUE(TileMapRuntimeStore::Get().LoadMapFromFile(m_Guid, m_Path));
    }

    void TearDown() override
    {
        TileMapRuntimeStore::Shutdown();
        std::error_code ec;
        std::filesystem::remove(m_Path, ec);
    }

    TileMapRuntimeStore& Store() { return TileMapRuntimeStore::Get(); }

    GUID m_Guid;
    std::filesystem::path m_Path;
};

} // namespace

TEST_F(TileMapRuntimeStoreTest, DecodesCellsAndGridFromTheAsset)
{
    const TileMap::TileMapData* data = Store().GetMap(m_Guid);
    ASSERT_NE(data, nullptr);
    EXPECT_FLOAT_EQ(data->CellWidth, 2.0f);
    EXPECT_FLOAT_EQ(data->CellHeight, 3.0f);
    ASSERT_EQ(data->Layers().size(), 2u);

    EXPECT_EQ(TileMap::CellTileId(Store().GetCell(m_Guid, kGroundLayerId, 0, 0)), 1u);
    const TileMap::TileCell flipped = Store().GetCell(m_Guid, kGroundLayerId, 3, 2);
    EXPECT_EQ(TileMap::CellTileId(flipped), 2u);
    EXPECT_TRUE(TileMap::CellFlipH(flipped));
    EXPECT_EQ(TileMap::CellTileId(Store().GetCell(m_Guid, kGroundLayerId, -1, -1)), 4u);
    EXPECT_TRUE(TileMap::CellIsEmpty(Store().GetCell(m_Guid, kGroundLayerId, 9, 9)));
}

TEST_F(TileMapRuntimeStoreTest, DecodeDirtiesEveryChunkExactlyOnce)
{
    std::vector<TileMap::ChunkCoord> chunks;
    ASSERT_TRUE(Store().ConsumeDirtyChunks(m_Guid, kGroundLayerId, chunks));
    // (0,0) holds two cells, plus (1,1) and (-1,-1).
    ASSERT_EQ(chunks.size(), 3u);
    EXPECT_EQ(chunks[0].X, -1);
    EXPECT_EQ(chunks[0].Y, -1);
    EXPECT_EQ(chunks[1].X, 0);
    EXPECT_EQ(chunks[1].Y, 0);
    EXPECT_EQ(chunks[2].X, 1);
    EXPECT_EQ(chunks[2].Y, 1);

    // Consuming drains: a second call reports nothing and clears the output.
    chunks.push_back(TileMap::ChunkCoord{99, 99});
    EXPECT_FALSE(Store().ConsumeDirtyChunks(m_Guid, kGroundLayerId, chunks));
    EXPECT_TRUE(chunks.empty());
}

TEST_F(TileMapRuntimeStoreTest, SetCellDirtiesOnlyTheTouchedChunk)
{
    std::vector<TileMap::ChunkCoord> chunks;
    ASSERT_TRUE(Store().ConsumeDirtyChunks(m_Guid, kGroundLayerId, chunks));

    ASSERT_TRUE(Store().SetCell(m_Guid, kGroundLayerId, 40, 41, MakeCell(9)));
    ASSERT_TRUE(Store().ConsumeDirtyChunks(m_Guid, kGroundLayerId, chunks));
    ASSERT_EQ(chunks.size(), 1u);
    EXPECT_EQ(chunks[0].X, 1);
    EXPECT_EQ(chunks[0].Y, 1);
    EXPECT_EQ(TileMap::CellTileId(Store().GetCell(m_Guid, kGroundLayerId, 40, 41)), 9u);
}

TEST_F(TileMapRuntimeStoreTest, CollisionLedgerDrainsIndependentlyOfTheRenderLedger)
{
    std::vector<TileMap::ChunkCoord> chunks;
    ASSERT_TRUE(Store().ConsumeDirtyChunks(m_Guid, kGroundLayerId, chunks));
    ASSERT_EQ(chunks.size(), 3u);
    // The render drain must not have consumed the collision ledger.
    ASSERT_TRUE(Store().ConsumeCollisionDirtyChunks(m_Guid, kGroundLayerId, chunks));
    EXPECT_EQ(chunks.size(), 3u);
    EXPECT_FALSE(Store().ConsumeCollisionDirtyChunks(m_Guid, kGroundLayerId, chunks));

    // One cell write re-arms both ledgers.
    ASSERT_TRUE(Store().SetCell(m_Guid, kGroundLayerId, 0, 0, MakeCell(7)));
    ASSERT_TRUE(Store().ConsumeCollisionDirtyChunks(m_Guid, kGroundLayerId, chunks));
    EXPECT_EQ(chunks.size(), 1u);
    ASSERT_TRUE(Store().ConsumeDirtyChunks(m_Guid, kGroundLayerId, chunks));
    EXPECT_EQ(chunks.size(), 1u);
}

TEST_F(TileMapRuntimeStoreTest, RepeatedEditsInOneChunkCoalesceToOneDirtyEntry)
{
    std::vector<TileMap::ChunkCoord> chunks;
    ASSERT_TRUE(Store().ConsumeDirtyChunks(m_Guid, kGroundLayerId, chunks));

    for (int32 x = 0; x < 8; ++x)
        ASSERT_TRUE(Store().SetCell(m_Guid, kGroundLayerId, x, 0, MakeCell(5)));

    ASSERT_TRUE(Store().ConsumeDirtyChunks(m_Guid, kGroundLayerId, chunks));
    EXPECT_EQ(chunks.size(), 1u);
}

TEST_F(TileMapRuntimeStoreTest, DirtyChunksArePerLayer)
{
    std::vector<TileMap::ChunkCoord> chunks;
    ASSERT_TRUE(Store().ConsumeDirtyChunks(m_Guid, kGroundLayerId, chunks));

    const TileMap::TileMapData* data = Store().GetMap(m_Guid);
    ASSERT_NE(data, nullptr);
    const uint32 decorLayerId = data->Layers()[1].Id;
    ASSERT_NE(decorLayerId, kGroundLayerId);

    ASSERT_TRUE(Store().SetCell(m_Guid, decorLayerId, 0, 0, MakeCell(7)));
    EXPECT_FALSE(Store().ConsumeDirtyChunks(m_Guid, kGroundLayerId, chunks));
    ASSERT_TRUE(Store().ConsumeDirtyChunks(m_Guid, decorLayerId, chunks));
    EXPECT_EQ(chunks.size(), 1u);
}

TEST_F(TileMapRuntimeStoreTest, EditVersionAdvancesOnlyOnAcceptedMutations)
{
    const uint64 afterLoad = Store().EditVersion(m_Guid);
    EXPECT_GT(afterLoad, 0u);

    ASSERT_TRUE(Store().SetCell(m_Guid, kGroundLayerId, 1, 1, MakeCell(6)));
    EXPECT_EQ(Store().EditVersion(m_Guid), afterLoad + 1);

    // Unknown layer: rejected, so the version must not move.
    EXPECT_FALSE(Store().SetCell(m_Guid, /*layerId*/ 999, 1, 1, MakeCell(6)));
    EXPECT_EQ(Store().EditVersion(m_Guid), afterLoad + 1);

    // Reads never advance it either.
    (void)Store().GetCell(m_Guid, kGroundLayerId, 1, 1);
    EXPECT_EQ(Store().EditVersion(m_Guid), afterLoad + 1);

    EXPECT_EQ(Store().EditVersion(GUID("99999999-8888-7777-6666-555555555555")), 0u);
}

TEST_F(TileMapRuntimeStoreTest, StructureVersionIgnoresCellWrites)
{
    const uint64 afterLoad = Store().StructureVersion(m_Guid);
    EXPECT_GT(afterLoad, 0u);

    // A stroke is many cell writes; a layer list must not rebuild for any of them.
    ASSERT_TRUE(Store().SetCell(m_Guid, kGroundLayerId, 1, 1, MakeCell(6)));
    ASSERT_TRUE(Store().SetCell(m_Guid, kGroundLayerId, 2, 1, MakeCell(6)));
    EXPECT_EQ(Store().StructureVersion(m_Guid), afterLoad);

    Store().MarkStructuralChange(m_Guid);
    EXPECT_EQ(Store().StructureVersion(m_Guid), afterLoad + 1);

    EXPECT_EQ(Store().StructureVersion(GUID("99999999-8888-7777-6666-555555555555")), 0u);
}

TEST_F(TileMapRuntimeStoreTest, MarkAllChunksDirtyReOffersEveryChunk)
{
    std::vector<TileMap::ChunkCoord> chunks;
    ASSERT_TRUE(Store().ConsumeDirtyChunks(m_Guid, kGroundLayerId, chunks));
    ASSERT_FALSE(Store().ConsumeDirtyChunks(m_Guid, kGroundLayerId, chunks));

    Store().MarkAllChunksDirty(m_Guid);
    ASSERT_TRUE(Store().ConsumeDirtyChunks(m_Guid, kGroundLayerId, chunks));
    EXPECT_EQ(chunks.size(), 3u);
}

TEST_F(TileMapRuntimeStoreTest, ReloadDropsInMemoryEditsAndReDirtiesOnReDecode)
{
    ASSERT_TRUE(Store().SetCell(m_Guid, kGroundLayerId, 0, 0, MakeCell(42)));
    EXPECT_EQ(TileMap::CellTileId(Store().GetCell(m_Guid, kGroundLayerId, 0, 0)), 42u);

    Store().ReloadMap(m_Guid);
    // The store no longer knows the map; re-decoding restores the on-disk cell
    // and re-offers every chunk to the renderer.
    EXPECT_EQ(Store().EditVersion(m_Guid), 0u);
    ASSERT_TRUE(Store().LoadMapFromFile(m_Guid, m_Path));
    EXPECT_EQ(TileMap::CellTileId(Store().GetCell(m_Guid, kGroundLayerId, 0, 0)), 1u);

    std::vector<TileMap::ChunkCoord> chunks;
    ASSERT_TRUE(Store().ConsumeDirtyChunks(m_Guid, kGroundLayerId, chunks));
    EXPECT_EQ(chunks.size(), 3u);
}

TEST_F(TileMapRuntimeStoreTest, UnknownMapYieldsNoDataAndRejectsWrites)
{
    const GUID unknown("00000000-1111-2222-3333-444444444444");
    EXPECT_EQ(Store().GetMap(unknown), nullptr);
    EXPECT_FALSE(Store().SetCell(unknown, kGroundLayerId, 0, 0, MakeCell(1)));
    EXPECT_TRUE(TileMap::CellIsEmpty(Store().GetCell(unknown, kGroundLayerId, 0, 0)));

    std::vector<TileMap::ChunkCoord> chunks;
    EXPECT_FALSE(Store().ConsumeDirtyChunks(unknown, kGroundLayerId, chunks));
}

TEST_F(TileMapRuntimeStoreTest, SaveWritesResidentEditsBackAndTheySurviveAReDecode)
{
    ASSERT_TRUE(Store().SetCell(m_Guid, kGroundLayerId, 5, 7, MakeCell(42, /*flipH=*/true)));

    const std::filesystem::path savePath = m_Path.parent_path() / "roundtrip.tilemap";
    ASSERT_TRUE(Store().SaveMapToFile(m_Guid, savePath));

    const GUID reloaded("00000000-9999-8888-7777-666666666666");
    ASSERT_TRUE(Store().LoadMapFromFile(reloaded, savePath));
    const TileMap::TileCell cell = Store().GetCell(reloaded, kGroundLayerId, 5, 7);
    EXPECT_EQ(TileMap::CellTileId(cell), 42u);
    EXPECT_TRUE(TileMap::CellFlipH(cell));
}

TEST_F(TileMapRuntimeStoreTest, SaveDropsChunksAnEraseLeftEmpty)
{
    // A chunk materializes on first write and must not persist once every cell
    // in it is empty again — otherwise erasing grows the file forever.
    ASSERT_TRUE(Store().SetCell(m_Guid, kGroundLayerId, 500, 500, MakeCell(9)));
    ASSERT_TRUE(Store().SetCell(m_Guid, kGroundLayerId, 500, 500, TileMap::kEmptyCell));

    const std::filesystem::path savePath = m_Path.parent_path() / "compacted.tilemap";
    ASSERT_TRUE(Store().SaveMapToFile(m_Guid, savePath));

    const GUID reloaded("00000000-5555-4444-3333-222222222222");
    ASSERT_TRUE(Store().LoadMapFromFile(reloaded, savePath));
    const TileMap::TileMapData* data = Store().GetMap(reloaded);
    ASSERT_NE(data, nullptr);
    const TileMap::TileMapLayerData* layer = data->FindLayer(kGroundLayerId);
    ASSERT_NE(layer, nullptr);
    // The three authored chunks survive; the erased far-away one does not.
    EXPECT_EQ(layer->SortedChunkCoords().size(), 3u);
}

TEST_F(TileMapRuntimeStoreTest, SaveRejectsAnUnknownMap)
{
    const GUID unknown("00000000-1111-2222-3333-444444444444");
    EXPECT_FALSE(Store().SaveMapToFile(unknown, m_Path.parent_path() / "nope.tilemap"));
}

TEST_F(TileMapRuntimeStoreTest, StructuralChangeReDirtiesEveryChunkAndBumpsTheVersion)
{
    std::vector<TileMap::ChunkCoord> chunks;
    ASSERT_TRUE(Store().ConsumeDirtyChunks(m_Guid, kGroundLayerId, chunks));
    const uint64 before = Store().EditVersion(m_Guid);

    Store().MarkStructuralChange(m_Guid);

    EXPECT_GT(Store().EditVersion(m_Guid), before);
    ASSERT_TRUE(Store().ConsumeDirtyChunks(m_Guid, kGroundLayerId, chunks));
    EXPECT_EQ(chunks.size(), 3u);
}

TEST_F(TileMapRuntimeStoreTest, AHandWriteClearsTheAutoFlagAndAnAutoWriteRestoresIt)
{
    auto& store = TileMapRuntimeStore::Get();
    TileMap::TileMapData* data = store.GetMap(m_Guid);
    ASSERT_NE(data, nullptr);
    const TileMap::TileMapLayerData* ground = data->FindLayer(kGroundLayerId);
    ASSERT_NE(ground, nullptr);

    ASSERT_TRUE(store.SetCellAuto(m_Guid, kGroundLayerId, 5, 5, MakeCell(9), true));
    EXPECT_TRUE(ground->IsAutoCell(5, 5));

    // Painting over a dressed cell by hand is what claims it from the rules.
    ASSERT_TRUE(store.SetCell(m_Guid, kGroundLayerId, 5, 5, MakeCell(10)));
    EXPECT_FALSE(ground->IsAutoCell(5, 5));
    EXPECT_EQ(store.GetCell(m_Guid, kGroundLayerId, 5, 5), MakeCell(10));
}

TEST_F(TileMapRuntimeStoreTest, PaintingIntGridValuesDressesTheLayersThatReadThem)
{
    auto& store = TileMapRuntimeStore::Get();
    TileMap::TileMapData* data = store.GetMap(m_Guid);
    ASSERT_NE(data, nullptr);

    const uint32 sourceId = data->AddLayer("Semantics", TileMap::TileMapLayerType::IntGrid);
    TileMap::TileMapLayerData& art = *data->FindLayer(kGroundLayerId);
    art.Rules.SourceLayerId = sourceId;

    TileMap::TileRule rule;
    rule.Id = 1;
    rule.PatternSize = 1;
    rule.Pattern.assign(1, TileMap::RulePatternCell{TileMap::RulePatternOp::RequireValue, 1});
    rule.Tiles.push_back(TileMap::RuleTileOutput{MakeCell(77), 1.0f});
    TileMap::TileRuleGroup group;
    group.Id = 1;
    group.Rules.push_back(std::move(rule));
    art.Rules.Groups.push_back(std::move(group));

    ASSERT_TRUE(store.SetIntGridValue(m_Guid, sourceId, 7, 7, 1));
    EXPECT_EQ(store.GetIntGridValue(m_Guid, sourceId, 7, 7), 1);

    std::vector<TileMapDressedChange> changes;
    ASSERT_TRUE(store.DressLayersFromSource(m_Guid, sourceId, {TileMap::CellCoord{7, 7}}, changes));
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0].LayerId, kGroundLayerId);
    EXPECT_EQ(changes[0].Change.After, MakeCell(77));
    EXPECT_TRUE(changes[0].Change.AutoAfter);
    EXPECT_EQ(store.GetCell(m_Guid, kGroundLayerId, 7, 7), MakeCell(77));

    // The dressed cell's chunk has to reach the renderer through the same ledger
    // a hand-painted cell uses, or the art appears only after some other edit.
    std::vector<TileMap::ChunkCoord> dirty;
    ASSERT_TRUE(store.ConsumeDirtyChunks(m_Guid, kGroundLayerId, dirty));
    EXPECT_NE(std::find(dirty.begin(), dirty.end(), TileMap::ChunkCoord{0, 0}), dirty.end());
}

TEST_F(TileMapRuntimeStoreTest, IntGridWritesAreRejectedOnATilesLayer)
{
    auto& store = TileMapRuntimeStore::Get();
    EXPECT_FALSE(store.SetIntGridValue(m_Guid, kGroundLayerId, 0, 0, 1));
    EXPECT_FALSE(store.SetIntGridValue(m_Guid, 999u, 0, 0, 1));
}

TEST_F(TileMapRuntimeStoreTest, SaveEchoDoesNotDiscardEditsMadeAfterTheSave)
{
    auto& store = TileMapRuntimeStore::Get();
    ASSERT_TRUE(store.SetCell(m_Guid, kGroundLayerId, 5, 5, MakeCell(9)));
    ASSERT_TRUE(store.SaveMapToFile(m_Guid, m_Path));

    // The edit that used to be lost: made after the write, before the watcher's
    // AssetModified for that same write drains.
    ASSERT_TRUE(store.SetCell(m_Guid, kGroundLayerId, 6, 6, MakeCell(10)));
    store.NotifyMapFileChanged(m_Guid); // the save's own echo
    store.DrainPendingReloads();

    EXPECT_EQ(store.GetCell(m_Guid, kGroundLayerId, 6, 6), MakeCell(10));
    EXPECT_EQ(store.GetCell(m_Guid, kGroundLayerId, 5, 5), MakeCell(9));
}

TEST_F(TileMapRuntimeStoreTest, ExternalEditAfterSaveStillReloads)
{
    auto& store = TileMapRuntimeStore::Get();
    ASSERT_TRUE(store.SetCell(m_Guid, kGroundLayerId, 5, 5, MakeCell(9)));
    ASSERT_TRUE(store.SaveMapToFile(m_Guid, m_Path));

    // A genuinely external edit: different bytes on disk than the save wrote.
    {
        std::ofstream out(m_Path, std::ios::binary | std::ios::app);
        out << "\n";
    }
    ASSERT_TRUE(store.SetCell(m_Guid, kGroundLayerId, 6, 6, MakeCell(10)));
    store.NotifyMapFileChanged(m_Guid);
    store.DrainPendingReloads();

    // The resident data was dropped for re-decode (headless: the lazy re-read
    // has no asset registry, so the observable effect is the empty read).
    EXPECT_EQ(store.GetCell(m_Guid, kGroundLayerId, 6, 6), TileMap::kEmptyCell);
}

TEST_F(TileMapRuntimeStoreTest, ATileSetReloadBumpsTheVersionDerivedStateIsKeyedOn)
{
    // A re-slice keeps every tile id and moves the UV rects behind them, so a
    // consumer caching derived tables (the renderer's GPU descriptors) has no
    // signal in the cells. This counter is that signal.
    auto& store = TileMapRuntimeStore::Get();
    const GUID tileSet("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee");
    const GUID other("99999999-8888-7777-6666-555555555555");
    EXPECT_EQ(store.TileSetVersion(tileSet), 0u);

    store.NotifyTileSetFileChanged(tileSet);
    store.DrainPendingReloads();
    EXPECT_EQ(store.TileSetVersion(tileSet), 1u);

    store.NotifyTileSetFileChanged(tileSet);
    store.DrainPendingReloads();
    EXPECT_EQ(store.TileSetVersion(tileSet), 2u);

    // Only the tileset that changed moves; a shared cache must not rebuild the
    // tables of every other tileset in the scene.
    EXPECT_EQ(store.TileSetVersion(other), 0u);
}

TEST_F(TileMapRuntimeStoreTest, IdentityMoveKeepsUnsavedCellsAndBothDirtyLedgers)
{
    const GUID replacement("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee");
    auto* original = Store().GetMap(m_Guid);
    ASSERT_TRUE(Store().SetCell(m_Guid, kGroundLayerId, 7, 8, MakeCell(99)));
    const auto editVersion = Store().EditVersion(m_Guid);
    const auto structureVersion = Store().StructureVersion(m_Guid);
    Store().NotifyGuidRemapped(m_Guid, replacement);
    Store().DrainPendingReloads();

    EXPECT_EQ(Store().GetMap(replacement), original);
    EXPECT_EQ(Store().GetCell(replacement, kGroundLayerId, 7, 8), MakeCell(99));
    EXPECT_EQ(Store().EditVersion(replacement), editVersion);
    EXPECT_EQ(Store().StructureVersion(replacement), structureVersion);
    std::vector<TileMap::ChunkCoord> chunks;
    EXPECT_TRUE(Store().ConsumeDirtyChunks(replacement, kGroundLayerId, chunks));
    EXPECT_TRUE(Store().ConsumeCollisionDirtyChunks(replacement, kGroundLayerId, chunks));
    EXPECT_EQ(Store().GetMap(m_Guid), nullptr); // no registry in this fixture
}

TEST_F(TileMapRuntimeStoreTest, IdentityChainKeepsSaveEchoSuppression)
{
    const GUID intermediate("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee");
    const GUID replacement("99999999-8888-7777-6666-555555555555");
    ASSERT_TRUE(Store().SaveMapToFile(m_Guid, m_Path));
    ASSERT_TRUE(Store().SetCell(m_Guid, kGroundLayerId, 7, 8, MakeCell(99)));
    Store().NotifyMapFileChanged(m_Guid);
    Store().NotifyGuidRemapped(m_Guid, intermediate);
    Store().NotifyGuidRemapped(intermediate, replacement);
    Store().DrainPendingReloads();
    EXPECT_EQ(Store().GetCell(replacement, kGroundLayerId, 7, 8), MakeCell(99));

    // The same identity must still recognize a genuine external write.
    {
        std::ofstream out(m_Path, std::ios::binary | std::ios::app);
        out << "\n";
    }
    Store().NotifyMapFileChanged(replacement);
    Store().DrainPendingReloads();
    EXPECT_EQ(Store().GetMap(replacement), nullptr);
}

TEST_F(TileMapRuntimeStoreTest, IdentityMoveReplacesSpeculativeDestinationAndAdvancesVersions)
{
    const GUID replacement("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee");
    ASSERT_TRUE(Store().LoadMapFromFile(replacement, m_Path));
    Store().MarkStructuralChange(replacement);
    const auto oldVersion = Store().EditVersion(replacement);
    const auto oldStructure = Store().StructureVersion(replacement);
    ASSERT_TRUE(Store().SetCell(m_Guid, kGroundLayerId, 7, 8, MakeCell(99)));
    Store().NotifyGuidRemapped(m_Guid, replacement);
    Store().DrainPendingReloads();
    EXPECT_EQ(Store().GetCell(replacement, kGroundLayerId, 7, 8), MakeCell(99));
    EXPECT_GT(Store().EditVersion(replacement), oldVersion);
    EXPECT_GT(Store().StructureVersion(replacement), oldStructure);
}

TEST_F(TileMapRuntimeStoreTest, IdentityMoveKeepsTileSetVersionAndPendingReload)
{
    const GUID original("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee");
    const GUID replacement("99999999-8888-7777-6666-555555555555");
    Store().NotifyTileSetFileChanged(original);
    Store().DrainPendingReloads();
    Store().NotifyTileSetFileChanged(original);
    Store().NotifyGuidRemapped(original, replacement);
    Store().DrainPendingReloads();
    EXPECT_EQ(Store().TileSetVersion(replacement), 2u);
    EXPECT_EQ(Store().TileSetVersion(original), 0u);
}

TEST_F(TileMapRuntimeStoreTest, IdentityMoveTracksRenamedFileForSaveEcho)
{
    const GUID replacement("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee");
    ASSERT_TRUE(Store().SaveMapToFile(m_Guid, m_Path));
    ASSERT_TRUE(Store().SetCell(m_Guid, kGroundLayerId, 7, 8, MakeCell(99)));
    const auto previousPath = m_Path;
    m_Path += ".renamed";
    std::filesystem::rename(previousPath, m_Path);
    Store().NotifyGuidRemapped(m_Guid, replacement, m_Path);
    Store().NotifyMapFileChanged(replacement);
    Store().DrainPendingReloads();
    EXPECT_EQ(Store().GetCell(replacement, kGroundLayerId, 7, 8), MakeCell(99));
}

TEST_F(TileMapRuntimeStoreTest, IdentityMoveKeepsResidentTileSetAndInvalidatesDerivedTables)
{
    const GUID original("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee");
    const GUID replacement("99999999-8888-7777-6666-555555555555");
    {
        std::ofstream out(m_Path, std::ios::trunc);
        out << R"({"tileWidthPx":16,"tileHeightPx":16,"tiles":[{"id":1}]})";
    }
    ASSERT_TRUE(Store().LoadTileSetFromFile(original, m_Path));
    const auto* tileSet = Store().GetTileSet(original);
    ASSERT_NE(tileSet, nullptr);
    Store().NotifyGuidRemapped(original, replacement);
    Store().DrainPendingReloads();
    EXPECT_EQ(Store().GetTileSet(replacement), tileSet);
    EXPECT_GT(Store().TileSetVersion(replacement), 0u);
    Store().NotifyTileSetFileChanged(replacement);
    Store().DrainPendingReloads();
    EXPECT_EQ(Store().GetTileSet(replacement), nullptr);
    EXPECT_EQ(Store().TileSetVersion(replacement), 2u);
}

TEST_F(TileMapRuntimeStoreTest, EditorMutationBeforeFrameDrainUsesRemappedResidentDocument)
{
    const GUID replacement("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee");
    ASSERT_TRUE(Store().SetCell(m_Guid, kGroundLayerId, 7, 8, MakeCell(99)));
    Store().NotifyGuidRemapped(m_Guid, replacement);
    ASSERT_TRUE(Store().SetCell(replacement, kGroundLayerId, 9, 8, MakeCell(100)));
    Store().DrainPendingReloads();
    EXPECT_EQ(Store().GetCell(replacement, kGroundLayerId, 7, 8), MakeCell(99));
    EXPECT_EQ(Store().GetCell(replacement, kGroundLayerId, 9, 8), MakeCell(100));
}
