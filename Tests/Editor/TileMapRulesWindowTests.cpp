#include "DebugServer/TileMapRulesWindow.h"
#include "TileMap/TileCellEncoding.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <limits>
#include <thread>

using namespace GameEngine;
using namespace GameEngine::Editor;
using namespace GameEngine::TileMap;
using nlohmann::json;

TEST(TileMapRulesWindowTests, EndpointResponseTerminatesAndPreservesAllChannels)
{
    // Run the real response builder in a bounded child: restoring the old int32
    // loops must produce a failed test rather than hanging the whole suite.
    ::testing::FLAGS_gtest_death_test_style = "threadsafe";
    ASSERT_EXIT(([&]
    {
        std::thread([]
        {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            std::_Exit(91);
        }).detach();
        TileMapLayerData layer;
        layer.Id = 7;
        const int32 x = std::numeric_limits<int32>::max();
        const int32 y = std::numeric_limits<int32>::min();
        layer.SetCell(x, y, MakeCell(19));
        layer.SetIntGridValue(x, y, 3);
        layer.SetAutoCell(x, y, true);
        json result;
        const bool ok = BuildTileMapRulesWindow(layer,
            json{{"x0", x}, {"x1", x}, {"y0", y}, {"y1", y}}, result);
        const json expected{{"layer", 7}, {"tileIds", {{19}}}, {"intGrid", {{3}}}, {"auto", {{1}}}};
        std::_Exit(ok && result == expected ? 0 : 92);
    }()), testing::ExitedWithCode(0), "");
}

TEST(TileMapRulesWindowTests, NormalResponseReadsRowsTopDownAndColumnsLeftToRight)
{
    TileMapLayerData layer;
    layer.Id = 2;
    layer.SetCell(-1, 1, MakeCell(11));
    layer.SetCell(0, 1, MakeCell(12));
    layer.SetCell(-1, 0, MakeCell(21));
    layer.SetCell(0, 0, MakeCell(22));
    layer.SetIntGridValue(0, 1, 4);
    layer.SetAutoCell(-1, 0, true);
    json result;
    ASSERT_TRUE(BuildTileMapRulesWindow(layer, {{"x0", -1}, {"x1", 0}, {"y0", 0}, {"y1", 1}}, result));
    EXPECT_EQ(result["tileIds"], (json{{11, 12}, {21, 22}}));
    EXPECT_EQ(result["intGrid"], (json{{0, 4}, {0, 0}}));
    EXPECT_EQ(result["auto"], (json{{0, 0}, {1, 0}}));
}

TEST(TileMapRulesWindowTests, RefusesExtremeSpansAndOverBudgetWithoutChangingResponse)
{
    TileMapLayerData layer;
    json result = "unchanged";
    const auto lo = std::numeric_limits<int32>::min();
    const auto hi = std::numeric_limits<int32>::max();
    for (const json& bounds : {json{{"x0", lo}, {"x1", hi}, {"y0", lo}, {"y1", hi}},
                              json{{"x0", 0}, {"x1", 64}, {"y0", 0}, {"y1", 64}},
                              json{{"x0", 1}, {"x1", 0}}})
    {
        EXPECT_FALSE(BuildTileMapRulesWindow(layer, bounds, result));
        EXPECT_EQ(result, "unchanged");
    }
    EXPECT_TRUE(BuildTileMapRulesWindow(layer, {{"x1", 63}, {"y1", 63}}, result));
}

TEST(TileMapRulesWindowTests, RefusesNonIntegerAndOutOfRangeBounds)
{
    TileMapLayerData layer;
    for (const json& value : {json(1.5), json("1"), json(true), json(nullptr),
                             json(static_cast<int64>(std::numeric_limits<int32>::max()) + 1),
                             json(static_cast<int64>(std::numeric_limits<int32>::min()) - 1),
                             json(std::numeric_limits<uint64>::max())})
    {
        for (const char* key : {"x0", "x1", "y0", "y1"})
        {
            json result = "unchanged";
            EXPECT_FALSE(BuildTileMapRulesWindow(layer, json{{key, value}}, result)) << key << ": " << value;
            EXPECT_EQ(result, "unchanged");
        }
    }
}
