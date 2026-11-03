#include "TileMap/TileMapData.h"
#include "TileMap/TileRuleEvaluator.h"
#include "TileMap/TileRules.h"

#include <gtest/gtest.h>

#include <limits>
#include <random>
#include <string>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::TileMap;

namespace
{

constexpr uint8 kWall = 1;
constexpr uint8 kWater = 2;

/**
 * @brief A map with one IntGrid layer feeding one Tiles layer — the shape every
 * rule test needs, so the tests can spell out only what differs.
 */
struct RuleFixture
{
    TileMapData Map;
    uint32 SourceId = 0;
    uint32 ArtId = 0;

    RuleFixture()
    {
        SourceId = Map.AddLayer("Semantics", TileMapLayerType::IntGrid);
        ArtId = Map.AddLayer("Art", TileMapLayerType::Tiles);
        Art().Rules.SourceLayerId = SourceId;
    }

    TileMapLayerData& Source() { return *Map.FindLayer(SourceId); }
    TileMapLayerData& Art() { return *Map.FindLayer(ArtId); }

    void AddGroup(std::vector<TileRule> rules)
    {
        TileRuleGroup group;
        group.Id = Art().Rules.NextGroupId++;
        group.Rules = std::move(rules);
        Art().Rules.Groups.push_back(std::move(group));
    }

    /** Fills a rectangle of the IntGrid, inclusive of both bounds. */
    void FillIntGrid(int32 x0, int32 y0, int32 x1, int32 y1, uint8 value)
    {
        for (int32 y = y0; y <= y1; ++y)
        {
            for (int32 x = x0; x <= x1; ++x)
                Source().SetIntGridValue(x, y, value);
        }
    }

    bool Evaluate()
    {
        std::vector<RuleCellChange> changed;
        return EvaluateRules(Map, ArtId, changed);
    }

    bool EvaluateIncremental(const std::vector<CellCoord>& dirty)
    {
        std::vector<RuleCellChange> changed;
        return EvaluateRulesIncremental(Map, ArtId, dirty, changed);
    }
};

/**
 * Builds a rule from a picture of its pattern, read top row first:
 * '.' don't care, '#' require `value`, 'o' forbid `value`, '*' require any.
 */
TileRule MakeRule(uint32 id, int32 size, const std::string& picture, uint8 value, uint32 tileId)
{
    TileRule rule;
    rule.Id = id;
    rule.PatternSize = size;
    rule.Pattern.assign(static_cast<size_t>(size) * static_cast<size_t>(size), RulePatternCell{});
    EXPECT_EQ(picture.size(), rule.Pattern.size());
    for (size_t i = 0; i < rule.Pattern.size() && i < picture.size(); ++i)
    {
        switch (picture[i])
        {
        case '#': rule.Pattern[i] = RulePatternCell{RulePatternOp::RequireValue, value}; break;
        case 'o': rule.Pattern[i] = RulePatternCell{RulePatternOp::ForbidValue, value}; break;
        case '*': rule.Pattern[i] = RulePatternCell{RulePatternOp::RequireAny, 0}; break;
        default: break;
        }
    }
    rule.Tiles.push_back(RuleTileOutput{MakeCell(tileId), 1.0f});
    return rule;
}

/** The dressed cells of a rectangle, row-major from the top, for golden compares. */
std::vector<TileCell> ReadPlane(TileMapLayerData& layer, int32 x0, int32 y0, int32 x1, int32 y1)
{
    std::vector<TileCell> plane;
    for (int32 y = y1; y >= y0; --y)
    {
        for (int32 x = x0; x <= x1; ++x)
            plane.push_back(layer.GetCell(x, y));
    }
    return plane;
}

} // namespace

TEST(TileRuleEvaluator, TheDressedPlaneMatchesTheGolden)
{
    RuleFixture fixture;
    fixture.FillIntGrid(0, 0, 2, 2, kWall);
    // "Nothing above me" wins over the plain interior rule, so the top row of the
    // block gets the cap tile and everything else the body tile.
    fixture.AddGroup({MakeRule(1, 3, ".o."
                                     ".#."
                                     "...",
                               kWall, 20),
                      MakeRule(2, 1, "#", kWall, 10)});

    ASSERT_TRUE(fixture.Evaluate());

    const std::vector<TileCell> expected{
        kEmptyCell,    kEmptyCell,    kEmptyCell,    kEmptyCell,    kEmptyCell,
        kEmptyCell,    MakeCell(20),  MakeCell(20),  MakeCell(20),  kEmptyCell,
        kEmptyCell,    MakeCell(10),  MakeCell(10),  MakeCell(10),  kEmptyCell,
        kEmptyCell,    MakeCell(10),  MakeCell(10),  MakeCell(10),  kEmptyCell,
        kEmptyCell,    kEmptyCell,    kEmptyCell,    kEmptyCell,    kEmptyCell};
    EXPECT_EQ(ReadPlane(fixture.Art(), -1, -1, 3, 3), expected);
}

TEST(TileRuleEvaluator, ANonBreakingMatchLetsALaterGroupPaintOver)
{
    RuleFixture fixture;
    fixture.FillIntGrid(0, 0, 0, 0, kWall);

    TileRule first = MakeRule(1, 1, "#", kWall, 10);
    first.BreakOnMatch = false;
    fixture.AddGroup({first});
    fixture.AddGroup({MakeRule(2, 1, "#", kWall, 11)});

    ASSERT_TRUE(fixture.Evaluate());
    EXPECT_EQ(fixture.Art().GetCell(0, 0), MakeCell(11));
}

TEST(TileRuleEvaluator, ABreakingMatchEndsTheCellOutright)
{
    RuleFixture fixture;
    fixture.FillIntGrid(0, 0, 0, 0, kWall);
    fixture.AddGroup({MakeRule(1, 1, "#", kWall, 10)}); // BreakOnMatch defaults on
    fixture.AddGroup({MakeRule(2, 1, "#", kWall, 11)});

    ASSERT_TRUE(fixture.Evaluate());
    EXPECT_EQ(fixture.Art().GetCell(0, 0), MakeCell(10));
}

TEST(TileRuleEvaluator, RequireAnyMatchesEveryNonZeroValue)
{
    RuleFixture fixture;
    fixture.Source().SetIntGridValue(0, 0, kWall);
    fixture.Source().SetIntGridValue(1, 0, kWater);
    fixture.AddGroup({MakeRule(1, 1, "*", 0, 10)});

    ASSERT_TRUE(fixture.Evaluate());
    EXPECT_EQ(fixture.Art().GetCell(0, 0), MakeCell(10));
    EXPECT_EQ(fixture.Art().GetCell(1, 0), MakeCell(10));
    EXPECT_EQ(fixture.Art().GetCell(2, 0), kEmptyCell);
}

TEST(TileRuleEvaluator, ForbidRejectsOnlyTheNamedValue)
{
    RuleFixture fixture;
    fixture.Source().SetIntGridValue(0, 0, kWall);
    fixture.Source().SetIntGridValue(0, 1, kWall);  // above the first cell
    fixture.Source().SetIntGridValue(1, 0, kWall);
    fixture.Source().SetIntGridValue(1, 1, kWater); // above the second cell

    // Forbid names the wall value, so a water neighbour above still matches.
    fixture.AddGroup({MakeRule(1, 3, ".o."
                                     ".#."
                                     "...",
                               kWall, 20)});

    ASSERT_TRUE(fixture.Evaluate());
    EXPECT_EQ(fixture.Art().GetCell(0, 0), kEmptyCell);
    EXPECT_EQ(fixture.Art().GetCell(1, 0), MakeCell(20));
}

TEST(TileRuleEvaluator, MirroringExpandsThePatternAndFlipsTheOutput)
{
    RuleFixture fixture;
    // Two walls, one with water on its right and one with water on its left.
    fixture.Source().SetIntGridValue(0, 0, kWall);
    fixture.Source().SetIntGridValue(1, 0, kWater);
    fixture.Source().SetIntGridValue(10, 0, kWall);
    fixture.Source().SetIntGridValue(9, 0, kWater);

    // "Water to my right" — only the X mirror can cover the opposite shore, and
    // it must flip the tile it paints.
    TileRule rule = MakeRule(1, 3, "..."
                                   ".#."
                                   "...",
                             kWall, 30);
    rule.Pattern[5] = RulePatternCell{RulePatternOp::RequireValue, kWater};
    rule.MirrorX = true;
    fixture.AddGroup({rule});

    ASSERT_TRUE(fixture.Evaluate());
    EXPECT_EQ(fixture.Art().GetCell(0, 0), MakeCell(30));
    EXPECT_EQ(fixture.Art().GetCell(10, 0), MakeCell(30, true));
    EXPECT_EQ(fixture.Art().GetCell(5, 0), kEmptyCell);
}

TEST(TileRuleEvaluator, ModuloRestrictsTheRuleToItsPhase)
{
    RuleFixture fixture;
    fixture.FillIntGrid(0, 0, 7, 0, kWall);

    TileRule sparse = MakeRule(1, 1, "#", kWall, 40);
    sparse.ModuloX = 3;
    sparse.OffsetX = 1;
    fixture.AddGroup({sparse});
    fixture.AddGroup({MakeRule(2, 1, "#", kWall, 10)});

    ASSERT_TRUE(fixture.Evaluate());
    for (int32 x = 0; x <= 7; ++x)
    {
        const bool onPhase = ((x - 1) % 3) == 0;
        EXPECT_EQ(fixture.Art().GetCell(x, 0), MakeCell(onPhase ? 40u : 10u)) << "x " << x;
    }
}

TEST(TileRuleEvaluator, ChanceZeroNeverFiresAndChanceOneAlwaysDoes)
{
    RuleFixture fixture;
    fixture.FillIntGrid(0, 0, 15, 0, kWall);

    TileRule never = MakeRule(1, 1, "#", kWall, 40);
    never.Chance = 0.0f;
    TileRule always = MakeRule(2, 1, "#", kWall, 10);
    always.Chance = 1.0f;
    fixture.AddGroup({never, always});

    ASSERT_TRUE(fixture.Evaluate());
    for (int32 x = 0; x <= 15; ++x)
        EXPECT_EQ(fixture.Art().GetCell(x, 0), MakeCell(10)) << "x " << x;
}

TEST(TileRuleEvaluator, APartialChanceSplitsTheCellsAndStaysPutUnderReevaluation)
{
    RuleFixture fixture;
    fixture.FillIntGrid(0, 0, 63, 0, kWall);

    TileRule sometimes = MakeRule(1, 1, "#", kWall, 40);
    sometimes.Chance = 0.5f;
    fixture.AddGroup({sometimes});
    fixture.AddGroup({MakeRule(2, 1, "#", kWall, 10)});

    ASSERT_TRUE(fixture.Evaluate());
    const std::vector<TileCell> first = ReadPlane(fixture.Art(), 0, 0, 63, 0);
    int32 decorated = 0;
    for (const TileCell cell : first)
    {
        if (CellTileId(cell) == 40u)
            ++decorated;
    }
    EXPECT_GT(decorated, 8);
    EXPECT_LT(decorated, 56);

    ASSERT_TRUE(fixture.Evaluate());
    EXPECT_EQ(ReadPlane(fixture.Art(), 0, 0, 63, 0), first);
}

TEST(TileRuleEvaluator, TheWeightedPickIsStablePerCellAcrossIncrementalPasses)
{
    RuleFixture fixture;
    fixture.FillIntGrid(0, 0, 31, 0, kWall);

    // A 3x3 pattern so an edit really does drag its neighbours back through the
    // evaluator — a 1x1 rule would pass this test by never re-evaluating them.
    TileRule variants = MakeRule(1, 3, "..."
                                       ".#."
                                       "...",
                                 kWall, 10);
    variants.Tiles.push_back(RuleTileOutput{MakeCell(11), 1.0f});
    variants.Tiles.push_back(RuleTileOutput{MakeCell(12), 1.0f});
    fixture.AddGroup({variants});

    ASSERT_TRUE(fixture.Evaluate());
    const std::vector<TileCell> settled = ReadPlane(fixture.Art(), 0, 0, 31, 0);

    // All three variants have to actually appear, or "stable" would be trivial.
    bool sawEach[3]{false, false, false};
    for (const TileCell cell : settled)
    {
        ASSERT_GE(CellTileId(cell), 10u);
        ASSERT_LE(CellTileId(cell), 12u);
        sawEach[CellTileId(cell) - 10u] = true;
    }
    EXPECT_TRUE(sawEach[0] && sawEach[1] && sawEach[2]);

    // Carving one cell out must not reshuffle the variants of its neighbours.
    fixture.Source().SetIntGridValue(10, 0, 0);
    ASSERT_TRUE(fixture.EvaluateIncremental({CellCoord{10, 0}}));
    EXPECT_EQ(fixture.Art().GetCell(10, 0), kEmptyCell);
    for (int32 x = 0; x <= 31; ++x)
    {
        if (x == 10)
            continue;
        EXPECT_EQ(fixture.Art().GetCell(x, 0), settled[static_cast<size_t>(x)]) << "x " << x;
    }
}

TEST(TileRuleEvaluator, HandPaintedCellsSurviveAReDress)
{
    RuleFixture fixture;
    fixture.FillIntGrid(0, 0, 4, 0, kWall);
    fixture.AddGroup({MakeRule(1, 1, "#", kWall, 10)});

    // A hand-painted detail: a tile with no auto flag on it.
    fixture.Art().SetCell(2, 0, MakeCell(99));

    ASSERT_TRUE(fixture.Evaluate());
    EXPECT_EQ(fixture.Art().GetCell(2, 0), MakeCell(99));
    EXPECT_EQ(fixture.Art().GetCell(1, 0), MakeCell(10));

    // And it still survives when the IntGrid under it is erased.
    fixture.Source().SetIntGridValue(2, 0, 0);
    ASSERT_TRUE(fixture.EvaluateIncremental({CellCoord{2, 0}}));
    EXPECT_EQ(fixture.Art().GetCell(2, 0), MakeCell(99));
}

TEST(TileRuleEvaluator, ErasingTheIntGridClearsTheDressing)
{
    RuleFixture fixture;
    fixture.FillIntGrid(0, 0, 2, 0, kWall);
    fixture.AddGroup({MakeRule(1, 1, "#", kWall, 10)});
    ASSERT_TRUE(fixture.Evaluate());
    ASSERT_EQ(fixture.Art().GetCell(1, 0), MakeCell(10));
    EXPECT_TRUE(fixture.Art().IsAutoCell(1, 0));

    fixture.Source().SetIntGridValue(1, 0, 0);
    ASSERT_TRUE(fixture.EvaluateIncremental({CellCoord{1, 0}}));
    EXPECT_EQ(fixture.Art().GetCell(1, 0), kEmptyCell);
    EXPECT_FALSE(fixture.Art().IsAutoCell(1, 0));
}

TEST(TileRuleEvaluator, AStampPaintsItsWholeFootprint)
{
    RuleFixture fixture;
    fixture.Source().SetIntGridValue(0, 0, kWall);

    TileRule stamp = MakeRule(1, 3, "..."
                                    ".#."
                                    "...",
                              kWall, 0);
    stamp.OutputMode = RuleOutputMode::Stamp;
    stamp.Tiles.clear();
    stamp.Stamp = {MakeCell(1), MakeCell(2), MakeCell(3),
                   MakeCell(4), MakeCell(5), MakeCell(6),
                   MakeCell(7), MakeCell(8), MakeCell(9)};
    fixture.AddGroup({stamp});

    ASSERT_TRUE(fixture.Evaluate());
    // Stamp row 0 is the topmost row, so tile 1 lands above-left of the centre.
    EXPECT_EQ(fixture.Art().GetCell(-1, 1), MakeCell(1));
    EXPECT_EQ(fixture.Art().GetCell(0, 0), MakeCell(5));
    EXPECT_EQ(fixture.Art().GetCell(1, -1), MakeCell(9));
}

TEST(TileRuleEvaluator, AnUnboundOrMistypedLayerIsARefusal)
{
    RuleFixture fixture;
    fixture.Art().Rules.SourceLayerId = 0;
    EXPECT_FALSE(fixture.Evaluate());

    fixture.Art().Rules.SourceLayerId = fixture.ArtId; // a Tiles layer, not IntGrid
    EXPECT_FALSE(fixture.Evaluate());

    std::vector<RuleCellChange> changed;
    EXPECT_FALSE(EvaluateRules(fixture.Map, 999u, changed));
}

TEST(TileRuleEvaluator, ChangedCellsAreReportedOnceAndOnlyWhenTheyMoved)
{
    RuleFixture fixture;
    fixture.FillIntGrid(0, 0, 1, 0, kWall);
    fixture.AddGroup({MakeRule(1, 1, "#", kWall, 10)});

    std::vector<RuleCellChange> changed;
    ASSERT_TRUE(EvaluateRules(fixture.Map, fixture.ArtId, changed));
    EXPECT_EQ(changed.size(), 2u);

    changed.clear();
    ASSERT_TRUE(EvaluateRules(fixture.Map, fixture.ArtId, changed));
    EXPECT_TRUE(changed.empty());
}

TEST(TileRuleEvaluator, IncrementalReproducesTheFullPassUnderRandomEdits)
{
    // Fixed seeds: a property test that reshuffles per run reports failures no
    // one can reproduce.
    for (uint32 trialSeed = 1; trialSeed <= 8u; ++trialSeed)
    {
        RuleFixture incremental;
        incremental.Art().Rules.Seed = trialSeed;
        incremental.AddGroup({MakeRule(1, 3, ".o."
                                             ".#."
                                             "...",
                                       kWall, 20),
                              MakeRule(2, 3, "..."
                                             ".#o"
                                             "...",
                                       kWall, 21),
                              MakeRule(3, 5, "....."
                                             "....."
                                             "..#.."
                                             "....."
                                             ".....",
                                       kWall, 10)});
        // A second value so the forbid/any ops see something other than blank.
        incremental.AddGroup({MakeRule(4, 1, "#", kWater, 50)});

        std::mt19937 rng(trialSeed);
        std::uniform_int_distribution<int32> coordinate(-6, 6);
        std::uniform_int_distribution<int32> pick(0, 2);

        for (int32 edit = 0; edit < 40; ++edit)
        {
            const int32 x = coordinate(rng);
            const int32 y = coordinate(rng);
            const int32 choice = pick(rng);
            const uint8 value = choice == 0 ? 0 : (choice == 1 ? kWall : kWater);
            incremental.Source().SetIntGridValue(x, y, value);
            ASSERT_TRUE(incremental.EvaluateIncremental({CellCoord{x, y}}));
        }

        // The same IntGrid and the same rules, dressed from scratch.
        RuleFixture full;
        full.Map = incremental.Map;
        full.Art().Clear();
        ASSERT_TRUE(full.Evaluate());

        for (int32 y = -12; y <= 12; ++y)
        {
            for (int32 x = -12; x <= 12; ++x)
            {
                ASSERT_EQ(incremental.Art().GetCell(x, y), full.Art().GetCell(x, y))
                    << "trial " << trialSeed << " cell " << x << "," << y;
            }
        }
    }
}

TEST(TileRuleEvaluatorTests, BoundaryHalosDoNotWrapIntoTheOppositeEdge)
{
    const int32 low = std::numeric_limits<int32>::min();
    const int32 high = std::numeric_limits<int32>::max();
    for (bool incremental : {false, true})
    {
        for (CellCoord edge : {CellCoord{high, high}, CellCoord{low, low}})
        {
            RuleFixture fixture;
            fixture.Source().SetIntGridValue(edge.X, edge.Y, kWall);
            // No source predicate: every halo cell is painted, exposing any
            // wrapped evaluation cell even where the source is empty.
            fixture.AddGroup({MakeRule(1, 3, ".........", kWall, 7)});
            ASSERT_TRUE(incremental ? fixture.EvaluateIncremental({edge}) : fixture.Evaluate());
            EXPECT_EQ(fixture.Art().GetCell(edge.X, edge.Y), MakeCell(7));
            const int32 opposite = edge.X == high ? low : high;
            EXPECT_EQ(fixture.Art().GetCell(opposite, edge.Y), kEmptyCell);
            EXPECT_EQ(fixture.Art().GetCell(edge.X, opposite), kEmptyCell);
            EXPECT_EQ(fixture.Art().GetCell(opposite, opposite), kEmptyCell);
        }
    }
}

TEST(TileRuleEvaluatorTests, BoundaryPatternReadsEmptyInsteadOfWrappedSource)
{
    const int32 low = std::numeric_limits<int32>::min();
    const int32 high = std::numeric_limits<int32>::max();
    RuleFixture fixture;
    fixture.Source().SetIntGridValue(high, 0, kWall);
    fixture.Source().SetIntGridValue(low, 0, kWall);
    // Right neighbour of INT_MAX must be empty even though INT_MIN is occupied.
    fixture.AddGroup({MakeRule(1, 3, "....##...", kWall, 7)});
    ASSERT_TRUE(fixture.EvaluateIncremental({{high, 0}}));
    EXPECT_EQ(fixture.Art().GetCell(high, 0), kEmptyCell);
    fixture.Art().Rules.Groups[0].Rules[0].Pattern[5] = {RulePatternOp::RequireValue, 0};
    ASSERT_TRUE(fixture.EvaluateIncremental({{high, 0}}));
    EXPECT_EQ(fixture.Art().GetCell(high, 0), MakeCell(7));
}

TEST(TileRuleEvaluatorTests, BoundaryStampDropsCellsOutsideTheDomain)
{
    const int32 high = std::numeric_limits<int32>::max();
    const int32 low = std::numeric_limits<int32>::min();
    RuleFixture fixture;
    fixture.Source().SetIntGridValue(high, high, kWall);
    auto rule = MakeRule(1, 3, "....#....", kWall, 7);
    rule.OutputMode = RuleOutputMode::Stamp;
    rule.Stamp.assign(9, MakeCell(8));
    fixture.AddGroup({rule});
    ASSERT_TRUE(fixture.EvaluateIncremental({{high, high}}));
    EXPECT_EQ(fixture.Art().GetCell(high, high), MakeCell(8));
    EXPECT_EQ(fixture.Art().GetCell(high - 1, high - 1), MakeCell(8));
    EXPECT_EQ(fixture.Art().GetCell(low, high), kEmptyCell);
    EXPECT_EQ(fixture.Art().GetCell(high, low), kEmptyCell);
    EXPECT_EQ(fixture.Art().GetCell(low, low), kEmptyCell);
}

TEST(TileRuleEvaluatorTests, ModuloUsesTheFullCellOffsetDifference)
{
    RuleFixture fixture;
    fixture.Source().SetIntGridValue(1, 1, kWall);
    auto rule = MakeRule(1, 1, "#", kWall, 7);
    rule.ModuloX = rule.ModuloY = 3;
    rule.OffsetX = rule.OffsetY = std::numeric_limits<int32>::min();
    fixture.AddGroup({rule});
    ASSERT_TRUE(fixture.EvaluateIncremental({{1, 1}}));
    // (1 - INT_MIN) == 2147483649, exactly divisible by three.
    EXPECT_EQ(fixture.Art().GetCell(1, 1), MakeCell(7));
}

TEST(TileRuleEvaluatorTests, LargeFiniteWeightsPreserveTheSameRelativeSelection)
{
    RuleFixture large;
    RuleFixture normalized;
    auto rule = MakeRule(1, 1, "#", kWall, 7);
    rule.Tiles.push_back({MakeCell(8), 1.0f});
    normalized.AddGroup({rule});
    for (auto& output : rule.Tiles)
        output.Weight = std::numeric_limits<float32>::max();
    large.AddGroup({rule});
    large.FillIntGrid(0, 0, 63, 0, kWall);
    normalized.FillIntGrid(0, 0, 63, 0, kWall);
    ASSERT_TRUE(large.Evaluate());
    ASSERT_TRUE(normalized.Evaluate());
    unsigned first = 0;
    unsigned second = 0;
    for (int32 x = 0; x < 64; ++x)
    {
        const auto cell = large.Art().GetCell(x, 0);
        EXPECT_EQ(cell, normalized.Art().GetCell(x, 0));
        first += cell == MakeCell(7);
        second += cell == MakeCell(8);
    }
    EXPECT_GT(first, 0u);
    EXPECT_GT(second, 0u);
}
