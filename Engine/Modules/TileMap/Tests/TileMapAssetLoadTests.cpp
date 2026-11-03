#include "TileMap/TileMapAsset.h"

#include <gtest/gtest.h>

#include <string>

using namespace GameEngine;
using namespace GameEngine::TileMap;

namespace
{

constexpr const char* kTileSetGuid = "11111111-2222-3333-4444-555555555555";

/** A 32-row chunk whose bottom row starts with `firstCells` and is otherwise empty. */
std::string ChunkRowsJson(const std::string& firstRow)
{
    std::string rows = "[\"" + firstRow + "\"";
    std::string emptyRow = "0";
    for (int32 x = 1; x < kChunkSize; ++x)
        emptyRow += ",0";
    for (int32 y = 1; y < kChunkSize; ++y)
        rows += ",\"" + emptyRow + "\"";
    return rows + "]";
}

std::string RowWithLeadingCells(const std::string& leading, int32 leadingCount)
{
    std::string row = leading;
    for (int32 x = leadingCount; x < kChunkSize; ++x)
        row += ",0";
    return row;
}

bool Parse(TileMapAsset& asset, const std::string& text)
{
    const Vector<uint8> bytes(text.begin(), text.end());
    return asset.LoadFromData(bytes);
}

std::string MapJson(const std::string& layers)
{
    return R"({"cellWidth": 0.5, "cellHeight": 0.25, "layers": [)" + layers + "]}";
}

} // namespace

TEST(TileMapAssetLoad, LoadsLayersCellsAndCellSize)
{
    // Chunk (-1, 2): negative chunk coordinates are part of the unbounded grid.
    const std::string layer = std::string(R"({"id": 7, "tileset": ")") + kTileSetGuid +
                              R"(", "chunks": [{"x": -1, "y": 2, "rows": )" +
                              ChunkRowsJson(RowWithLeadingCells("3,0,536870913", 3)) + "}]}";
    TileMapAsset asset(GUID(), "test://map.tilemap");
    ASSERT_TRUE(Parse(asset, MapJson(layer)));

    const TileMapData& data = asset.Data();
    EXPECT_FLOAT_EQ(data.CellWidth, 0.5f);
    EXPECT_FLOAT_EQ(data.CellHeight, 0.25f);
    ASSERT_EQ(data.Layers().size(), 1u);
    const TileMapLayerData* loaded = data.FindLayer(7);
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->TileSetGuid, GUID(kTileSetGuid));

    const TileMapChunk* chunk = loaded->FindChunk(ChunkCoord{-1, 2});
    ASSERT_NE(chunk, nullptr);
    EXPECT_EQ(chunk->Cells[0], 3u);
    EXPECT_TRUE(CellIsEmpty(chunk->Cells[1]));
    // Flip bits ride above the 29-bit id and survive the load untouched.
    EXPECT_EQ(chunk->Cells[2], 536870913u);
    EXPECT_EQ(CellTileId(chunk->Cells[2]), 1u);
    EXPECT_EQ(loaded->FindChunk(ChunkCoord{0, 0}), nullptr);
}

TEST(TileMapAssetLoad, SortedChunkCoordsWalkRowsBottomUp)
{
    const std::string empty = ChunkRowsJson(RowWithLeadingCells("1", 1));
    const std::string layer = R"({"id": 1, "chunks": [{"x": 3, "y": 1, "rows": )" + empty +
                              R"(}, {"x": -2, "y": 1, "rows": )" + empty + R"(}, {"x": 9, "y": -4, "rows": )" +
                              empty + "}]}";
    TileMapAsset asset(GUID(), "test://map.tilemap");
    ASSERT_TRUE(Parse(asset, MapJson(layer)));

    const std::vector<ChunkCoord> coords = asset.Data().FindLayer(1)->SortedChunkCoords();
    ASSERT_EQ(coords.size(), 3u);
    EXPECT_EQ(coords[0], (ChunkCoord{9, -4}));
    EXPECT_EQ(coords[1], (ChunkCoord{-2, 1}));
    EXPECT_EQ(coords[2], (ChunkCoord{3, 1}));
}

TEST(TileMapAssetLoad, MalformedChunkRowsAreRejected)
{
    const std::string shortRow = R"({"id": 1, "chunks": [{"x": 0, "y": 0, "rows": )" + ChunkRowsJson("1,2,3") + "}]}";
    const std::string trailingGarbage =
        R"({"id": 1, "chunks": [{"x": 0, "y": 0, "rows": )" + ChunkRowsJson(RowWithLeadingCells("1", 1) + ",x") + "}]}";
    const std::string wideCell =
        R"({"id": 1, "chunks": [{"x": 0, "y": 0, "rows": )" + ChunkRowsJson(RowWithLeadingCells("4294967296", 1)) + "}]}";
    const std::string missingRows = R"({"id": 1, "chunks": [{"x": 0, "y": 0, "rows": ["0"]}]})";

    for (const std::string& layer : {shortRow, trailingGarbage, wideCell, missingRows})
    {
        TileMapAsset asset(GUID(), "test://map.tilemap");
        EXPECT_FALSE(Parse(asset, MapJson(layer))) << layer.substr(0, 80);
        EXPECT_TRUE(asset.Data().Layers().empty());
        EXPECT_EQ(asset.GetState(), AssetState::Failed);
    }
}

TEST(TileMapAssetLoad, LayerIdsMustBeAssignableAndUnique)
{
    TileMapAsset zeroId(GUID(), "test://map.tilemap");
    EXPECT_FALSE(Parse(zeroId, MapJson(R"({"id": 0})")));

    TileMapAsset negativeId(GUID(), "test://map.tilemap");
    EXPECT_FALSE(Parse(negativeId, MapJson(R"({"id": -3})")));

    TileMapAsset duplicate(GUID(), "test://map.tilemap");
    EXPECT_FALSE(Parse(duplicate, MapJson(R"({"id": 4}, {"id": 4})")));
}

TEST(TileMapAssetLoad, ChunkCoordinatesOutsideInt32AreRejected)
{
    const std::string layer = R"({"id": 1, "chunks": [{"x": 4294967296, "y": 0, "rows": )" +
                              ChunkRowsJson(RowWithLeadingCells("1", 1)) + "}]}";
    TileMapAsset asset(GUID(), "test://map.tilemap");
    EXPECT_FALSE(Parse(asset, MapJson(layer)));
}

TEST(TileMapAssetLoad, MistypedCellSizeReadsAsTheDefault)
{
    TileMapAsset asset(GUID(), "test://map.tilemap");
    ASSERT_TRUE(Parse(asset, R"({"cellWidth": "wide", "layers": []})"));
    EXPECT_FLOAT_EQ(asset.Data().CellWidth, 1.0f);
}

TEST(TileMapAssetLoad, WrongTypesAndBrokenJsonFailWithoutThrowing)
{
    for (const char* text : {"{", "[]", R"({"layers": 3})", R"({"cellWidth": 0, "layers": []})",
                             R"({"layers": [{"id": "one"}]})"})
    {
        TileMapAsset asset(GUID(), "test://map.tilemap");
        EXPECT_FALSE(Parse(asset, text)) << text;
    }
}

TEST(TileMapAssetLoad, AuthoredIdsDoNotSelectAnEarlierLayerDuringLoading)
{
    const std::string first = R"({"id":2,"name":"A","chunks":[{"x":0,"y":0,"rows":)" +
        ChunkRowsJson(RowWithLeadingCells("3", 1)) + "}]}";
    const std::string second = R"({"id":3,"name":"B","chunks":[{"x":0,"y":0,"rows":)" +
        ChunkRowsJson(RowWithLeadingCells("5", 1)) + "}]}";
    TileMapAsset asset(GUID(), "test://ids.tilemap");
    ASSERT_TRUE(Parse(asset, MapJson(first + "," + second)));
    ASSERT_NE(asset.Data().FindLayer(2), nullptr);
    ASSERT_NE(asset.Data().FindLayer(3), nullptr);
    EXPECT_EQ(asset.Data().FindLayer(2)->Name, "A");
    EXPECT_EQ(asset.Data().FindLayer(3)->Name, "B");
    EXPECT_EQ(asset.Data().FindLayer(2)->GetCell(0, 0), 3u);
    EXPECT_EQ(asset.Data().FindLayer(3)->GetCell(0, 0), 5u);
}

TEST(TileMapAssetLoad, RejectsCellSizeThatOverflowsFloat)
{
    TileMapAsset asset(GUID(), "test://size.tilemap");
    EXPECT_FALSE(Parse(asset, R"({"cellWidth":1e100,"cellHeight":1,"layers":[]})"));
}

TEST(TileMapAssetLoad, RejectsChunksOutsideRepresentableCellCoordinates)
{
    TileMapAsset asset(GUID(), "test://coords.tilemap");
    for (const auto coordinate : {"2147483647", "-2147483648"})
    {
        const std::string layer = std::string(R"({"id":1,"chunks":[{"x":)") + coordinate +
            R"(,"y":0,"rows":)" + ChunkRowsJson(RowWithLeadingCells("1", 1)) + "}]}";
        EXPECT_FALSE(Parse(asset, MapJson(layer)));
    }
}

TEST(TileMapAssetLoad, ExhaustedLayerIdsDoNotWrapOrOverwriteExistingLayers)
{
    TileMapData data;
    const auto existing = data.AddLayer("Existing", TileMapLayerType::Tiles);
    data.RestoreNextLayerId(UINT32_MAX);
    EXPECT_EQ(data.AddLayer("Overflow", TileMapLayerType::Tiles), 0u);
    EXPECT_EQ(data.DuplicateLayer(existing, "Copy"), 0u);
    ASSERT_EQ(data.Layers().size(), 1u);
    EXPECT_EQ(data.FindLayer(existing)->Name, "Existing");
    EXPECT_EQ(data.NextLayerId(), UINT32_MAX);
}

TEST(TileMapAssetLoad, RejectsRuleFloatsOutsideFiniteStorageRange)
{
    for (const char* field : {R"("chance":1e100)", R"("tiles":[{"cell":1,"weight":1e100}])"})
    {
        TileMapAsset asset(GUID(), "test://rule.tilemap");
        const std::string layer = std::string(R"({"id":1,"rules":{"groups":[{"rules":[{"patternSize":1,"pattern":["0"],)") +
            field + "}]}]}}";
        EXPECT_FALSE(Parse(asset, MapJson(layer)));
    }
}
