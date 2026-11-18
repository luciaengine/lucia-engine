#include "Engine/Rendering/VirtualShadows/VirtualShadowPageCache.h"

#include <algorithm>
#include <array>
#include <set>
#include <span>
#include <vector>

#include <gtest/gtest.h>

namespace GameEngine::Engine::Renderer
{
namespace
{

constexpr VirtualShadowPage kSun{7, VirtualShadowLight::Directional, 3, 0, -17, 11};
constexpr VirtualShadowPage kSpot{7, VirtualShadowLight::Spot, 0, 1, 5, 9};
constexpr VirtualShadowPage kPoint{7, VirtualShadowLight::Point, 0, 1, 5, 9};

TEST(VirtualShadowPageCacheTests, AllLightFamiliesShareTilesWithoutAliasingTheirAddresses)
{
    VirtualShadowPageCache cache(9);
    auto sunNextLevel = kSun;
    ++sunNextLevel.Projection;
    auto pointAdjacentFace = kPoint;
    ++pointAdjacentFace.Projection;
    auto pointCoarse = kPoint;
    ++pointCoarse.Mip;
    auto distantSunPage = kSun;
    distantSunPage.X += int64_t{1} << 32;
    auto anotherSpot = kSpot;
    ++anotherSpot.LightId;
    auto anotherPoint = kPoint;
    ++anotherPoint.LightId;
    const std::array pages{kSun, kSpot, kPoint, sunNextLevel, pointAdjacentFace, pointCoarse,
                           distantSunPage, anotherSpot, anotherPoint, kPoint};

    const auto cold = cache.Plan(pages, 1, 0);
    ASSERT_EQ(cold.size(), 9u);
    std::set<uint32_t> tiles;
    for (const auto& row : cold)
    {
        EXPECT_TRUE(row.NeedsRender);
        EXPECT_TRUE(tiles.insert(row.Tile).second);
        EXPECT_TRUE(cache.MarkRendered(row));
    }
    const auto warm = cache.Plan(pages, 2, 0);
    ASSERT_EQ(warm.size(), cold.size());
    for (std::size_t index = 0; index < warm.size(); ++index)
    {
        EXPECT_EQ(warm[index].Page, cold[index].Page);
        EXPECT_EQ(warm[index].Tile, cold[index].Tile);
        EXPECT_FALSE(warm[index].NeedsRender);
    }
}

TEST(VirtualShadowPageCacheTests, CapacityProtectsRequestedPagesAndPrioritizesNewRequests)
{
    VirtualShadowPageCache cache(2);
    const auto warm = cache.Plan(std::array{kPoint}, 1, 0);
    ASSERT_EQ(warm.size(), 1u);
    ASSERT_TRUE(cache.MarkRendered(warm[0]));

    const auto saturated = cache.Plan(std::array{kSpot, kPoint, kSun}, 2, 1);
    ASSERT_EQ(saturated.size(), 3u);
    EXPECT_NE(saturated[0].Tile, kUnmappedShadowPage);
    EXPECT_EQ(saturated[1].Tile, warm[0].Tile);
    EXPECT_FALSE(saturated[1].NeedsRender);
    EXPECT_EQ(saturated[2].Tile, kUnmappedShadowPage);
}

TEST(VirtualShadowPageCacheTests, HigherPriorityPageCanReplaceAStillVisiblePageAfterRetirement)
{
    VirtualShadowPageCache cache(1);
    const auto low = cache.Plan(std::array{kPoint}, 1, 0);
    ASSERT_EQ(low.size(), 1u);
    ASSERT_TRUE(cache.MarkRendered(low[0]));

    const std::array demand{kSun, kPoint};
    const auto blocked = cache.Plan(demand, 2, 0);
    ASSERT_EQ(blocked.size(), 2u);
    EXPECT_EQ(blocked[0].Tile, kUnmappedShadowPage);
    EXPECT_EQ(blocked[1].Tile, kUnmappedShadowPage);

    const auto promoted = cache.Plan(demand, 3, 1);
    ASSERT_EQ(promoted.size(), 2u);
    EXPECT_EQ(promoted[0].Tile, low[0].Tile);
    EXPECT_TRUE(promoted[0].NeedsRender);
    EXPECT_EQ(promoted[1].Tile, kUnmappedShadowPage);
}

TEST(VirtualShadowPageCacheTests, EvictionWaitsForRetirementAndReusesTheLeastRecentTile)
{
    VirtualShadowPageCache cache(2);
    const auto spot = cache.Plan(std::array{kSpot}, 1, 0);
    const auto point = cache.Plan(std::array{kPoint}, 2, 0);
    ASSERT_EQ(spot.size(), 1u);
    ASSERT_EQ(point.size(), 1u);
    ASSERT_NE(spot[0].Tile, point[0].Tile);

    const auto blocked = cache.Plan(std::array{kSun}, 3, 0);
    ASSERT_EQ(blocked.size(), 1u);
    EXPECT_EQ(blocked[0].Tile, kUnmappedShadowPage);

    const auto reused = cache.Plan(std::array{kSun}, 4, 2);
    ASSERT_EQ(reused.size(), 1u);
    EXPECT_EQ(reused[0].Tile, spot[0].Tile);
    EXPECT_TRUE(reused[0].NeedsRender);
    EXPECT_FALSE(cache.MarkRendered(spot[0]));
    EXPECT_TRUE(cache.MarkRendered(reused[0]));
}

TEST(VirtualShadowPageCacheTests, InvalidationRejectsStaleSubmissionAndLeavesOtherLightsWarm)
{
    VirtualShadowPageCache cache(3);
    const std::array pages{kSun, kSpot, kPoint};
    const auto first = cache.Plan(pages, 1, 0);
    ASSERT_EQ(first.size(), 3u);
    for (const auto& row : first)
        ASSERT_TRUE(cache.MarkRendered(row));

    cache.MarkDirty(kSpot);
    const auto dirty = cache.Plan(pages, 2, 1);
    ASSERT_EQ(dirty.size(), 3u);
    EXPECT_TRUE(dirty[1].NeedsRender);
    cache.InvalidateLight(VirtualShadowLight::Spot, kSpot.LightId);
    EXPECT_FALSE(cache.MarkRendered(dirty[1]));

    const auto retry = cache.Plan(pages, 3, 2);
    ASSERT_EQ(retry.size(), 3u);
    EXPECT_FALSE(retry[0].NeedsRender);
    EXPECT_TRUE(retry[1].NeedsRender);
    EXPECT_FALSE(retry[2].NeedsRender);
    EXPECT_EQ(retry[1].Tile, first[1].Tile);
    EXPECT_TRUE(cache.MarkRendered(retry[1]));
}

TEST(VirtualShadowPageCacheTests, InvalidPointFaceAndLocalMipCoordinatesDoNotConsumeTiles)
{
    auto badFace = kPoint;
    badFace.Projection = 6;
    auto badMip = kSpot;
    badMip.Mip = 7; // The coarsest mip has only page (0, 0).
    auto badClip = kSun;
    badClip.Projection = kVirtualShadowClipLevels;
    VirtualShadowPageCache cache(1);
    const auto work = cache.Plan(std::array{badFace, badMip, badClip, kSun}, 1, 0);
    ASSERT_EQ(work.size(), 1u);
    EXPECT_EQ(work[0].Page, kSun);
    EXPECT_EQ(work[0].Tile, 0u);
}

std::vector<VirtualShadowPage> SunRow(int64_t first, uint32_t count, uint8_t level = 0)
{
    std::vector<VirtualShadowPage> pages;
    for (uint32_t index = 0; index < count; ++index)
    {
        auto page = kSun;
        page.Projection = level;
        page.X = first + index;
        pages.push_back(page);
    }
    return pages;
}

std::vector<int64_t> RenderAll(VirtualShadowPageCache& cache, std::span<const VirtualShadowPageWork> rows)
{
    std::vector<int64_t> rendered;
    for (const auto& row : rows)
    {
        EXPECT_NE(row.Tile, kUnmappedShadowPage);
        if (row.NeedsRender && cache.MarkRendered(row))
            rendered.push_back(row.Page.X);
    }
    return rendered;
}

TEST(VirtualShadowPageCacheTests, DeformedPagesStayPublishedAndTheFinestLevelRefreshesEveryFrame)
{
    VirtualShadowPageCache cache(64);
    auto pages = SunRow(0, 4, 0);
    const auto coarse = SunRow(100, 40, 3);
    pages.insert(pages.end(), coarse.begin(), coarse.end());
    ASSERT_EQ(RenderAll(cache, cache.Plan(pages, 1, 0)).size(), pages.size());

    std::set<int64_t> coarseRefreshed;
    for (uint64_t frame = 2; frame <= 3; ++frame)
    {
        cache.MarkDeformed();
        const auto rows = cache.Plan(pages, frame, frame - 1);
        EXPECT_EQ(std::ranges::count_if(rows, [](const VirtualShadowPageWork& row) { return !row.NeedsRender; }),
                  static_cast<std::ptrdiff_t>(pages.size() - kVirtualShadowDeformedRefreshBudget));
        const auto rendered = RenderAll(cache, rows);
        ASSERT_EQ(rendered.size(), kVirtualShadowDeformedRefreshBudget);
        EXPECT_EQ(std::vector<int64_t>(rendered.begin(), rendered.begin() + 4), (std::vector<int64_t>{0, 1, 2, 3}));
        coarseRefreshed.insert(rendered.begin() + 4, rendered.end());
    }
    EXPECT_EQ(coarseRefreshed.size(), 2 * (kVirtualShadowDeformedRefreshBudget - 4));

    uint64_t frame = 4;
    while (!RenderAll(cache, cache.Plan(pages, frame, frame - 1)).empty() && frame < 8)
        ++frame;
    EXPECT_LT(frame, 8u);
}

TEST(VirtualShadowPageCacheTests, ADeformedPageOverItsAgeLimitRefreshesBeforeFinerPages)
{
    VirtualShadowPageCache cache(64);
    auto pages = SunRow(0, kVirtualShadowDeformedRefreshBudget + 4, 0);
    pages.push_back(SunRow(500, 1, 1).front());
    ASSERT_EQ(RenderAll(cache, cache.Plan(pages, 1, 0)).size(), pages.size());

    constexpr uint64_t kLevelOneLimit = kVirtualShadowDeformedMaxAgeFrames << 1;
    uint64_t refreshedAt = 0;
    for (uint64_t frame = 2; frame <= 1 + kLevelOneLimit && refreshedAt == 0; ++frame)
    {
        cache.MarkDeformed();
        const auto rendered = RenderAll(cache, cache.Plan(pages, frame, frame - 1));
        if (std::ranges::find(rendered, 500) != rendered.end())
            refreshedAt = frame;
    }
    EXPECT_EQ(refreshedAt, 1 + kLevelOneLimit);
}

TEST(VirtualShadowPageCacheTests, MissingPagesTakeTheRenderBudgetBeforeDeformedPages)
{
    VirtualShadowPageCache cache(64);
    const auto resident = SunRow(0, 10);
    ASSERT_EQ(RenderAll(cache, cache.Plan(resident, 1, 0)).size(), resident.size());

    cache.MarkDeformed();
    auto demand = SunRow(100, kVirtualShadowPageRenderBudget - 3);
    demand.insert(demand.end(), resident.begin(), resident.end());
    const auto rows = cache.Plan(demand, 2, 1);
    const auto refreshed = std::ranges::count_if(rows, [](const VirtualShadowPageWork& row)
                                                 { return row.NeedsRender && row.Page.X < 100; });
    EXPECT_EQ(refreshed, 3);
    EXPECT_EQ(std::ranges::count_if(rows, &VirtualShadowPageWork::NeedsRender), kVirtualShadowPageRenderBudget);
}

TEST(VirtualShadowPageCacheTests, ARefreshedPageKeepsItsTileContentAndANewPageDoesNot)
{
    VirtualShadowPageCache cache(64);
    const auto resident = SunRow(0, 4);
    ASSERT_EQ(RenderAll(cache, cache.Plan(resident, 1, 0)).size(), resident.size());

    cache.MarkDeformed();
    auto demand = SunRow(100, 2);
    demand.insert(demand.end(), resident.begin(), resident.end());
    for (const auto& row : cache.Plan(demand, 2, 1))
    {
        ASSERT_TRUE(row.NeedsRender);
        EXPECT_EQ(row.Refresh, row.Page.X < 100) << "page " << row.Page.X;
    }
}

TEST(VirtualShadowPageCacheTests, CropMapsTopLeftAtlasCoordinatesWithoutChangingPerspectiveDepth)
{
    constexpr float kDepth = 2.0f;
    constexpr float kPiOverTwo = 1.57079632679f;
    const auto projection = Mathematics::Matrix4x4::PerspectiveReverseZ(kPiOverTwo, 1.0f, 0.1f, 100.0f);
    const auto crop = CropVirtualShadowProjection(projection, 4, 1, 0);
    ASSERT_TRUE(crop);
    const auto expectCorner = [&](float x, float y, float expectedX, float expectedY) {
        const Mathematics::Vector4 position(x, y, kDepth, 1.0f);
        const auto original = projection.Transform(position);
        const auto page = crop->Transform(position);
        EXPECT_NEAR(page.x, expectedX * page.w, 1.0e-6f);
        EXPECT_NEAR(page.y, expectedY * page.w, 1.0e-6f);
        EXPECT_FLOAT_EQ(page.z, original.z);
        EXPECT_FLOAT_EQ(page.w, original.w);
    };

    expectCorner(-1.0f, 2.0f, -1.0f, 1.0f);
    expectCorner(0.0f, 1.0f, 1.0f, -1.0f);
    EXPECT_FALSE(CropVirtualShadowProjection(projection, 0, 0, 0));
    EXPECT_FALSE(CropVirtualShadowProjection(projection, 4, 0, 4));
}

} // namespace
} // namespace GameEngine::Engine::Renderer
