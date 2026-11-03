#include "DebugServer/TileMapRulesCommands.h"
#include "DebugServer/DebugServerReply.h"
#include "TileMap/TileMapEditContext.h"
#include "TileMap/TileMapAsset.h"
#include "TileMap/TileCellEncoding.h"
#include "TileMapECS/TileMapRuntimeStore.h"
#include "TestTempDir.h"

#include <gtest/gtest.h>
#include <fstream>
#include <limits>

using namespace GameEngine;
using namespace GameEngine::Editor;
using nlohmann::json;

class TileMapRulesCommands : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        const auto path = m_Temp.Path() / "rules.tilemap";
        TileMap::TileMapAsset asset(m_Guid, path);
        auto& data = asset.Data();
        const auto source = data.AddLayer("Values", TileMap::TileMapLayerType::IntGrid);
        const auto target = data.AddLayer("Dressed", TileMap::TileMapLayerType::Tiles);
        ASSERT_EQ(source, 1u);
        ASSERT_EQ(target, 2u);
        data.FindLayer(source)->SetIntGridValue(0, 0, 1);
        auto& rules = data.FindLayer(target)->Rules;
        rules.SourceLayerId = source;
        rules.Seed = 11;
        rules.NextGroupId = 2;
        TileMap::TileRuleGroup group;
        group.Id = 1;
        group.Name = "Existing";
        rules.Groups.push_back(group);
        Vector<uint8> bytes;
        ASSERT_TRUE(asset.SaveToData(bytes));
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        out.close();
        ASSERT_TRUE(m_Store.LoadMapFromFile(m_Guid, path));
        m_Context.SetTarget(ECS::EntityHandle{}, m_Guid, source);
    }

    json Request() const
    {
        json rule{{"pattern", {"=1"}}, {"tiles", {7}}};
        json group{{"name", "Replacement"}, {"rules", json::array({rule})}};
        return {{"rulesLayer", 2}, {"sourceLayer", 1}, {"seed", 12},
                {"groups", json::array({group})}};
    }

    std::string Snapshot()
    {
        TileMap::TileMapAsset asset(m_Guid, m_Temp.Path() / "snapshot.tilemap");
        asset.Data() = *m_Store.GetMap(m_Guid);
        Vector<uint8> bytes;
        EXPECT_TRUE(asset.SaveToData(bytes));
        return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }

    void CheckInvalidField(const char* field)
    {
        const auto before = Snapshot();
        const json invalid[] = {-1, 4294967297ULL, 4294967298ULL, std::numeric_limits<uint64>::max(),
            2.5, "1", nullptr, true};
        for (const json& value : invalid)
        {
            auto params = Request();
            params[field] = value;
            json reply;
            EXPECT_NO_THROW(reply = AuthorTileMapRules(&m_Context, &m_Store, params)) << field << ':' << value;
            EXPECT_TRUE(IsRefusal(reply)) << field << ':' << value << " reply=" << reply;
            if (IsRefusal(reply))
                EXPECT_NE(HandlerResponse("request", reply)["error"].get<std::string>().find(field), std::string::npos);
            EXPECT_EQ(Snapshot(), before) << field << ':' << value;
        }
    }

    TestUtils::ScopedTempDir m_Temp{TestUtils::MakeUniqueTempDirectory("tile-rule-command")};
    GUID m_Guid{"11111111-2222-3333-4444-555555555555"};
    TileMapECS::TileMapRuntimeStore m_Store;
    TileMapEditContext m_Context;
};

TEST_F(TileMapRulesCommands, RejectsInvalidRuleLayerBeforeNarrowing)
{
    CheckInvalidField("rulesLayer");
}
TEST_F(TileMapRulesCommands, RejectsInvalidSourceLayerBeforeClearingRules)
{
    CheckInvalidField("sourceLayer");
}
TEST_F(TileMapRulesCommands, RejectsInvalidSeedBeforeClearingRules)
{
    CheckInvalidField("seed");
}
TEST_F(TileMapRulesCommands, AuthorsAndDressesThroughTheRuntimeStore)
{
    auto params = Request();
    params["seed"] = std::numeric_limits<uint32>::max();
    const auto result = AuthorTileMapRules(&m_Context, &m_Store, params);
    ASSERT_FALSE(IsRefusal(result)) << result;
    EXPECT_EQ(result["dressedCells"], 1);
    EXPECT_EQ(m_Store.GetCell(m_Guid, 2, 0, 0), TileMap::MakeCell(7));
    EXPECT_TRUE(m_Store.GetMap(m_Guid)->FindLayer(2)->IsAutoCell(0, 0));
    EXPECT_EQ(m_Store.GetMap(m_Guid)->FindLayer(2)->Rules.Seed, std::numeric_limits<uint32>::max());
    const auto described = DescribeTileMapRules(&m_Context, &m_Store,
        {{"layer", 2}, {"window", {{"x0", 0}, {"x1", 0}, {"y0", 0}, {"y1", 0}}}});
    ASSERT_FALSE(IsRefusal(described)) << described;
    EXPECT_EQ(described["window"]["tileIds"], (json{{7}}));
    EXPECT_EQ(described["window"]["auto"], (json{{1}}));
    std::vector<TileMap::ChunkCoord> dirty;
    EXPECT_TRUE(m_Store.ConsumeDirtyChunks(m_Guid, 2, dirty));
    EXPECT_FALSE(dirty.empty());
}

TEST_F(TileMapRulesCommands, OmittedSourceAndSeedAreRetainedWhenAppending)
{
    auto params = Request();
    params.erase("sourceLayer");
    params.erase("seed");
    params["clearRules"] = false;
    const auto reply = AuthorTileMapRules(&m_Context, &m_Store, params);
    ASSERT_FALSE(IsRefusal(reply)) << reply;
    const auto& rules = m_Store.GetMap(m_Guid)->FindLayer(2)->Rules;
    EXPECT_EQ(rules.SourceLayerId, 1u);
    EXPECT_EQ(rules.Seed, 11u);
    ASSERT_EQ(rules.Groups.size(), 2u);
    EXPECT_EQ(rules.Groups[0].Name, "Existing");
    EXPECT_EQ(rules.Groups[1].Name, "Replacement");
}
