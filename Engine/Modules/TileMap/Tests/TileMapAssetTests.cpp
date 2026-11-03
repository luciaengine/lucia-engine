#include "TileMap/TileMapAsset.h"

#include <gtest/gtest.h>

#include <string>

using namespace GameEngine;
using namespace GameEngine::TileMap;

namespace
{

void FillTestMap(TileMapData& map)
{
    map.CellWidth = 0.5f;
    map.CellHeight = 0.5f;

    const uint32 groundId = map.AddLayer("Ground", TileMapLayerType::Tiles);
    const uint32 semanticsId = map.AddLayer("Semantics", TileMapLayerType::IntGrid);

    auto& ground = *map.FindLayer(groundId);
    ground.TileSetGuid = GUID::Generate();
    ground.SetCell(0, 0, MakeCell(1));
    ground.SetCell(1, 0, MakeCell(2, true));
    ground.SetCell(-40, 17, MakeCell(3, false, true, true));

    auto& semantics = *map.FindLayer(semanticsId);
    semantics.SetIntGridValue(0, 0, 1);
    semantics.SetIntGridValue(-40, 17, 200);
}

} // namespace

TEST(TileMapAssetJson, RoundTripPreservesLayersAndCells)
{
    TileMapAsset asset(GUID::Generate(), "test.tilemap");
    FillTestMap(asset.Data());

    Vector<uint8> data;
    ASSERT_TRUE(asset.SaveToData(data));

    TileMapAsset loaded(GUID::Generate(), "loaded.tilemap");
    ASSERT_TRUE(loaded.LoadFromData(data));

    const TileMapData& map = loaded.Data();
    EXPECT_FLOAT_EQ(map.CellWidth, 0.5f);
    EXPECT_FLOAT_EQ(map.CellHeight, 0.5f);
    ASSERT_EQ(map.Layers().size(), 2u);

    const TileMapLayerData& ground = map.Layers()[0];
    EXPECT_EQ(ground.Id, 1u);
    EXPECT_EQ(ground.Name, "Ground");
    EXPECT_EQ(ground.Type, TileMapLayerType::Tiles);
    EXPECT_EQ(ground.TileSetGuid, asset.Data().Layers()[0].TileSetGuid);
    EXPECT_EQ(ground.GetCell(0, 0), MakeCell(1));
    EXPECT_EQ(ground.GetCell(1, 0), MakeCell(2, true));
    EXPECT_EQ(ground.GetCell(-40, 17), MakeCell(3, false, true, true));
    EXPECT_EQ(ground.GetCell(2, 0), kEmptyCell);

    const TileMapLayerData& semantics = map.Layers()[1];
    EXPECT_EQ(semantics.Type, TileMapLayerType::IntGrid);
    EXPECT_EQ(semantics.GetIntGridValue(0, 0), 1);
    EXPECT_EQ(semantics.GetIntGridValue(-40, 17), 200);

    // Layer id counter survives so future layers get fresh ids.
    EXPECT_EQ(loaded.Data().NextLayerId(), 3u);
}

TEST(TileMapAssetJson, SerializationIsByteStable)
{
    TileMapAsset asset(GUID::Generate(), "test.tilemap");
    FillTestMap(asset.Data());

    Vector<uint8> first;
    ASSERT_TRUE(asset.SaveToData(first));

    TileMapAsset loaded(GUID::Generate(), "loaded.tilemap");
    ASSERT_TRUE(loaded.LoadFromData(first));
    Vector<uint8> second;
    ASSERT_TRUE(loaded.SaveToData(second));

    EXPECT_EQ(first, second);
}

TEST(TileMapAssetJson, CellSizeIsWrittenAtFloatPrecisionAndStillRoundTrips)
{
    // 0.2f has no exact double: assigned straight into the JSON it dumps as
    // 0.20000000298023224. The digits are representation error, not accuracy.
    TileMapAsset asset(GUID::Generate(), "test.tilemap");
    asset.Data().CellWidth = 0.2f;
    asset.Data().CellHeight = 0.2f;

    Vector<uint8> data;
    ASSERT_TRUE(asset.SaveToData(data));
    const std::string text(data.begin(), data.end());

    EXPECT_EQ(text.find("0.2000000"), std::string::npos)
        << "a cell size must not carry the double expansion of a float32";
    EXPECT_NE(text.find("\"cellWidth\": 0.2"), std::string::npos) << "0.2f must read as 0.2";

    TileMapAsset loaded(GUID::Generate(), "loaded.tilemap");
    ASSERT_TRUE(loaded.LoadFromData(data));
    EXPECT_EQ(loaded.Data().CellWidth, 0.2f);
    EXPECT_EQ(loaded.Data().CellHeight, 0.2f);
}

TEST(TileMapAssetJson, SingleCellEditTouchesFewLines)
{
    TileMapAsset asset(GUID::Generate(), "test.tilemap");
    FillTestMap(asset.Data());

    Vector<uint8> before;
    ASSERT_TRUE(asset.SaveToData(before));

    asset.Data().Layers()[0].SetCell(2, 0, MakeCell(5));
    Vector<uint8> after;
    ASSERT_TRUE(asset.SaveToData(after));

    // Cheap structural diff: count differing lines between the two serializations.
    const auto splitLines = [](const Vector<uint8>& data)
    {
        std::vector<std::string> lines;
        std::string current;
        for (const uint8 byte : data)
        {
            if (byte == '\n')
            {
                lines.push_back(std::move(current));
                current.clear();
            }
            else
            {
                current.push_back(static_cast<char>(byte));
            }
        }
        lines.push_back(std::move(current));
        return lines;
    };

    const auto beforeLines = splitLines(before);
    const auto afterLines = splitLines(after);
    ASSERT_EQ(beforeLines.size(), afterLines.size());
    size_t differing = 0;
    for (size_t i = 0; i < beforeLines.size(); ++i)
    {
        if (beforeLines[i] != afterLines[i])
            ++differing;
    }
    // The edited cell lives on one CSV row of one chunk.
    EXPECT_EQ(differing, 1u);
}

TEST(TileMapAssetJson, MalformedChunkRowFailsCleanly)
{
    const std::string bad = R"({
        "schemaVersion": 1,
        "layers": [{
            "id": 1, "name": "L", "type": "tiles",
            "chunks": [{"x": 0, "y": 0, "rows": ["1,2,3"]}]
        }]
    })";
    TileMapAsset asset(GUID::Generate(), "bad.tilemap");
    const Vector<uint8> data(bad.begin(), bad.end());
    EXPECT_FALSE(asset.LoadFromData(data));
    EXPECT_EQ(asset.GetState(), AssetState::Failed);
    EXPECT_TRUE(asset.Data().Layers().empty());
}

TEST(TileMapAssetJson, EmptyMapRoundTrips)
{
    TileMapAsset asset(GUID::Generate(), "empty.tilemap");
    Vector<uint8> data;
    ASSERT_TRUE(asset.SaveToData(data));

    TileMapAsset loaded(GUID::Generate(), "loaded.tilemap");
    ASSERT_TRUE(loaded.LoadFromData(data));
    EXPECT_TRUE(loaded.Data().Layers().empty());
    EXPECT_FLOAT_EQ(loaded.Data().CellWidth, 1.0f);
}

TEST(TileMapAssetJson, RulesIntGridValuesAndProvenanceRoundTrip)
{
    TileMapAsset asset(GUID::Generate(), "rules.tilemap");
    TileMapData& map = asset.Data();
    const uint32 semanticsId = map.AddLayer("Semantics", TileMapLayerType::IntGrid);
    const uint32 artId = map.AddLayer("Art", TileMapLayerType::Tiles);

    TileMapLayerData& semantics = *map.FindLayer(semanticsId);
    semantics.IntGridValues.push_back(IntGridValueDef{1, "Wall", 0xFF2277AAu});
    semantics.IntGridValues.push_back(IntGridValueDef{7, "Water", 0xFF884422u});
    semantics.SetIntGridValue(3, -2, 7);

    TileMapLayerData& art = *map.FindLayer(artId);
    art.Rules.SourceLayerId = semanticsId;
    art.Rules.Seed = 99u;
    art.Rules.NextGroupId = 2u;
    art.Rules.NextRuleId = 3u;

    TileRuleGroup group;
    group.Id = 1u;
    group.Name = "Walls";
    group.Enabled = false;

    TileRule weighted;
    weighted.Id = 1u;
    weighted.Name = "Body";
    weighted.PatternSize = 3;
    weighted.Pattern.assign(9, RulePatternCell{});
    weighted.Pattern[1] = RulePatternCell{RulePatternOp::ForbidValue, 1};
    weighted.Pattern[4] = RulePatternCell{RulePatternOp::RequireValue, 1};
    weighted.Pattern[7] = RulePatternCell{RulePatternOp::RequireAny, 0};
    weighted.Tiles.push_back(RuleTileOutput{MakeCell(10), 2.5f});
    weighted.Tiles.push_back(RuleTileOutput{MakeCell(11, true), 1.0f});
    weighted.Chance = 0.25f;
    weighted.BreakOnMatch = false;
    weighted.MirrorX = true;
    weighted.ModuloY = 3;
    weighted.OffsetY = 2;
    group.Rules.push_back(std::move(weighted));

    TileRule stamped;
    stamped.Id = 2u;
    stamped.Name = "Tree";
    stamped.PatternSize = 1;
    stamped.Pattern.assign(1, RulePatternCell{RulePatternOp::RequireValue, 7});
    stamped.OutputMode = RuleOutputMode::Stamp;
    stamped.Stamp.assign(1, MakeCell(42, false, true));
    group.Rules.push_back(std::move(stamped));
    art.Rules.Groups.push_back(std::move(group));

    art.SetCell(3, -2, MakeCell(10));
    art.SetAutoCell(3, -2, true);

    Vector<uint8> data;
    ASSERT_TRUE(asset.SaveToData(data));

    TileMapAsset loaded(GUID::Generate(), "loaded.tilemap");
    ASSERT_TRUE(loaded.LoadFromData(data));

    const TileMapLayerData& loadedSemantics = loaded.Data().Layers()[0];
    ASSERT_EQ(loadedSemantics.IntGridValues.size(), 2u);
    EXPECT_EQ(loadedSemantics.IntGridValues[1].Value, 7);
    EXPECT_EQ(loadedSemantics.IntGridValues[1].Name, "Water");
    EXPECT_EQ(loadedSemantics.IntGridValues[1].ColorRGBA, 0xFF884422u);
    ASSERT_NE(loadedSemantics.FindIntGridValue(1), nullptr);
    EXPECT_EQ(loadedSemantics.FindIntGridValue(2), nullptr);

    const TileMapLayerData& loadedArt = loaded.Data().Layers()[1];
    EXPECT_EQ(loadedArt.Rules.SourceLayerId, semanticsId);
    EXPECT_EQ(loadedArt.Rules.Seed, 99u);
    EXPECT_EQ(loadedArt.Rules.NextGroupId, 2u);
    EXPECT_EQ(loadedArt.Rules.NextRuleId, 3u);
    ASSERT_EQ(loadedArt.Rules.Groups.size(), 1u);

    const TileRuleGroup& loadedGroup = loadedArt.Rules.Groups[0];
    EXPECT_EQ(loadedGroup.Name, "Walls");
    EXPECT_FALSE(loadedGroup.Enabled);
    ASSERT_EQ(loadedGroup.Rules.size(), 2u);

    const TileRule& loadedWeighted = loadedGroup.Rules[0];
    EXPECT_EQ(loadedWeighted.Pattern[1], (RulePatternCell{RulePatternOp::ForbidValue, 1}));
    EXPECT_EQ(loadedWeighted.Pattern[4], (RulePatternCell{RulePatternOp::RequireValue, 1}));
    EXPECT_EQ(loadedWeighted.Pattern[7], (RulePatternCell{RulePatternOp::RequireAny, 0}));
    EXPECT_EQ(loadedWeighted.Pattern[0], RulePatternCell{});
    ASSERT_EQ(loadedWeighted.Tiles.size(), 2u);
    EXPECT_EQ(loadedWeighted.Tiles[0], (RuleTileOutput{MakeCell(10), 2.5f}));
    EXPECT_EQ(loadedWeighted.Tiles[1], (RuleTileOutput{MakeCell(11, true), 1.0f}));
    EXPECT_FLOAT_EQ(loadedWeighted.Chance, 0.25f);
    EXPECT_FALSE(loadedWeighted.BreakOnMatch);
    EXPECT_TRUE(loadedWeighted.MirrorX);
    EXPECT_FALSE(loadedWeighted.MirrorY);
    EXPECT_EQ(loadedWeighted.ModuloY, 3);
    EXPECT_EQ(loadedWeighted.OffsetY, 2);

    const TileRule& loadedStamped = loadedGroup.Rules[1];
    EXPECT_EQ(loadedStamped.OutputMode, RuleOutputMode::Stamp);
    ASSERT_EQ(loadedStamped.Stamp.size(), 1u);
    EXPECT_EQ(loadedStamped.Stamp[0], MakeCell(42, false, true));

    EXPECT_TRUE(loadedArt.IsAutoCell(3, -2));
    EXPECT_FALSE(loadedArt.IsAutoCell(4, -2));

    // Byte stability has to survive the new blocks too.
    Vector<uint8> second;
    ASSERT_TRUE(loaded.SaveToData(second));
    EXPECT_EQ(data, second);
}

TEST(TileMapAssetJson, ASchemaVersionOneFileStillLoads)
{
    // A .tilemap exactly as builds before the rule schema wrote it: no
    // intValues, no rules, no autoChunks anywhere.
    const std::string legacy = R"({
  "assetType": "TileMap",
  "cellWidth": 0.5,
  "cellHeight": 0.25,
  "nextLayerId": 3,
  "schemaVersion": 1,
  "layers": [
    {
      "id": 1,
      "name": "Ground",
      "type": "tiles",
      "tileset": "00000000-0000-0000-0000-000000000000",
      "chunks": [{"x": 0, "y": 0, "rows": [")" +
                              [] {
                                  std::string rows;
                                  for (int i = 0; i < 32; ++i)
                                  {
                                      if (i != 0)
                                          rows += "\", \"";
                                      for (int x = 0; x < 32; ++x)
                                      {
                                          if (x != 0)
                                              rows += ",";
                                          rows += (i == 0 && x == 0) ? "7" : "0";
                                      }
                                  }
                                  return rows;
                              }() +
                              R"("]}]
    },
    {
      "id": 2,
      "name": "Semantics",
      "type": "intgrid",
      "tileset": "00000000-0000-0000-0000-000000000000"
    }
  ]
})";

    TileMapAsset asset(GUID::Generate(), "legacy.tilemap");
    const Vector<uint8> data(legacy.begin(), legacy.end());
    ASSERT_TRUE(asset.LoadFromData(data));
    ASSERT_EQ(asset.Data().Layers().size(), 2u);

    const TileMapLayerData& ground = asset.Data().Layers()[0];
    EXPECT_EQ(ground.GetCell(0, 0), MakeCell(7));
    EXPECT_FALSE(ground.IsAutoCell(0, 0));
    EXPECT_TRUE(ground.Rules.IsEmpty());
    EXPECT_EQ(ground.Rules.SourceLayerId, 0u);
    EXPECT_TRUE(asset.Data().Layers()[1].IntGridValues.empty());
    EXPECT_FLOAT_EQ(asset.Data().CellWidth, 0.5f);
    EXPECT_EQ(asset.Data().NextLayerId(), 3u);
}

namespace
{

/** A .tilemap around one layer body, for hostile-input variants. */
std::string MapWithLayer(const std::string& layerBody)
{
    return R"({"schemaVersion":1,"assetType":"TileMap","layers":[)" + layerBody + "]}";
}

/** A chunk row of kChunkSize copies of `value`, plus optional trailing text. */
std::string ChunkRow(const std::string& value, const std::string& trailing = std::string())
{
    std::string row;
    for (int32 x = 0; x < kChunkSize; ++x)
    {
        if (x != 0)
            row.push_back(',');
        row += value;
    }
    return row + trailing;
}

/** A one-chunk `plane` block whose every row is `row`. */
std::string ChunkWithRow(const std::string& row, const char* plane)
{
    std::string rows;
    for (int32 y = 0; y < kChunkSize; ++y)
    {
        if (y != 0)
            rows += ",";
        rows += "\"" + row + "\"";
    }
    return std::string("\"") + plane + R"(":[{"x":0,"y":0,"rows":[)" + rows + "]}]";
}

bool LoadText(TileMapAsset& asset, const std::string& text)
{
    const Vector<uint8> data(text.begin(), text.end());
    return asset.LoadFromData(data);
}

} // namespace

TEST(TileMapAssetJson, TrailingGarbageAfterAChunkRowIsRejected)
{
    // A row holds exactly kChunkSize values; anything after them is corruption.
    TileMapAsset asset(GUID::Generate(), "trailing.tilemap");
    EXPECT_FALSE(LoadText(asset, MapWithLayer(R"({"id":1,"name":"L","type":"tiles",)" +
                                              ChunkWithRow(ChunkRow("0", ",7"), "chunks") + "}")));
    EXPECT_EQ(asset.GetState(), AssetState::Failed);
}

TEST(TileMapAssetJson, IntGridValuesTooLargeForTheirTypeAreRejected)
{
    // Silent truncation would turn 999 into 231 — a different authored value.
    TileMapAsset asset(GUID::Generate(), "truncate.tilemap");
    EXPECT_FALSE(LoadText(asset, MapWithLayer(R"({"id":1,"name":"L","type":"intgrid",)" +
                                              ChunkWithRow(ChunkRow("999"), "intChunks") + "}")));
    EXPECT_EQ(asset.GetState(), AssetState::Failed);
}

TEST(TileMapAssetJson, DuplicateLayerIdsAreRejected)
{
    // FindLayer returns the first match, so a duplicate id makes the second
    // layer unaddressable.
    TileMapAsset asset(GUID::Generate(), "dupes.tilemap");
    EXPECT_FALSE(LoadText(asset, MapWithLayer(R"({"id":4,"name":"A","type":"tiles"},)"
                                              R"({"id":4,"name":"B","type":"tiles"})")));
    EXPECT_EQ(asset.GetState(), AssetState::Failed);
    EXPECT_TRUE(asset.Data().Layers().empty());
}

TEST(TileMapAssetJson, NextLayerIdSaturatesInsteadOfWrapping)
{
    // The counter must stay above every id in the file. At the very top of the
    // range there is no next id, so the file is rejected rather than loaded
    // with a counter that has wrapped back to 1.
    TileMapAsset atMax(GUID::Generate(), "atmax.tilemap");
    EXPECT_FALSE(LoadText(atMax, MapWithLayer(R"({"id":4294967295,"name":"A","type":"tiles"})")));

    TileMapAsset belowMax(GUID::Generate(), "belowmax.tilemap");
    ASSERT_TRUE(
        LoadText(belowMax, MapWithLayer(R"({"id":4294967294,"name":"A","type":"tiles"})")));
    EXPECT_EQ(belowMax.Data().NextLayerId(), 4294967295u);
}

TEST(TileMapAssetJson, EmbeddedNulsAreStrippedFromNames)
{
    // A NUL truncates every C-string consumer downstream (UI labels, file names).
    TileMapAsset asset(GUID::Generate(), "nul.tilemap");
    ASSERT_TRUE(
        LoadText(asset, MapWithLayer(R"({"id":1,"name":"Gro\u0000und","type":"tiles"})")));
    ASSERT_EQ(asset.Data().Layers().size(), 1u);
    EXPECT_EQ(asset.Data().Layers()[0].Name, "Ground");
}

TEST(TileMapAssetJson, WrongFieldTypesFailWithoutThrowing)
{
    // The loader must not depend on exceptions: catch(const std::exception&)
    // is dead in builds whose translation units disagree about RTTI.
    const char* const hostile[] = {
        R"({"cellWidth":"1"})",
        R"({"layers":{"id":1}})",
        R"({"layers":[{"id":"1"}]})",
        R"({"layers":[{"id":1,"name":7}]})",
        R"({"layers":[{"id":1,"chunks":[{"x":"0","y":0,"rows":[]}]}]})",
        R"({"layers":[{"id":1,"intValues":[{"value":"3"}]}]})",
        R"({"layers":[{"id":1,"autoChunks":[{"x":0,"y":0,"rows":["a"]}]}]})",
        R"({"layers":[{"id":1,"rules":{"groups":[{"rules":[{"patternSize":"3"}]}]}}]})",
        R"({"nextLayerId":"7"})",
    };
    for (const char* text : hostile)
    {
        TileMapAsset asset(GUID::Generate(), "types.tilemap");
        EXPECT_NO_THROW({ (void)LoadText(asset, text); }) << text;
    }
}

TEST(TileMapAssetJson, LockRoundTripsAndIsOmittedWhenUnlocked)
{
    TileMapAsset asset(GUID::Generate(), "locked.tilemap");
    FillTestMap(asset.Data());
    asset.Data().Layers()[0].Locked = true;

    Vector<uint8> data;
    ASSERT_TRUE(asset.SaveToData(data));
    const std::string text(reinterpret_cast<const char*>(data.data()), data.size());
    // One locked layer of two: the unlocked one must not carry the key at all,
    // so a hand-edited .tilemap does not fill with "locked": false.
    EXPECT_EQ(text.find("\"locked\""), text.rfind("\"locked\""));
    EXPECT_NE(text.find("\"locked\""), std::string::npos);

    TileMapAsset loaded(GUID::Generate(), "loaded.tilemap");
    ASSERT_TRUE(loaded.LoadFromData(data));
    ASSERT_EQ(loaded.Data().Layers().size(), 2u);
    EXPECT_TRUE(loaded.Data().Layers()[0].Locked);
    EXPECT_FALSE(loaded.Data().Layers()[1].Locked);
}
