#include "Engine/Rendering/VirtualShadows/VirtualShadowPageCache.h"

#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <span>
#include <tuple>
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

TEST(VirtualShadowPageCacheTests, NoDeformedPageWaitsForeverAndNearPagesRefreshMoreOften)
{
    VirtualShadowPageCache cache(256);
    auto pages = SunRow(0, 128, 0);
    const auto coarse = SunRow(1000, 128, 2);
    pages.insert(pages.end(), coarse.begin(), coarse.end());
    ASSERT_EQ(RenderAll(cache, cache.Plan(pages, 1, 0)).size(), pages.size());

    std::map<int64_t, uint64_t> lastRender;
    std::map<int64_t, uint64_t> longestWait;
    for (const auto& page : pages)
        lastRender[page.X] = 1;
    constexpr uint64_t kFrames = 400;
    for (uint64_t frame = 2; frame <= kFrames; ++frame)
    {
        cache.MarkDeformed();
        for (const int64_t x : RenderAll(cache, cache.Plan(pages, frame, frame - 1)))
        {
            longestWait[x] = std::max(longestWait[x], frame - lastRender[x]);
            lastRender[x] = frame;
        }
    }
    for (const auto& [x, frame] : lastRender)
        EXPECT_GT(frame, kFrames / 2) << "page " << x << " stopped refreshing";
    EXPECT_LT(longestWait[0], longestWait[127]) << "the nearest fine page waits less than the farthest";
    EXPECT_LT(longestWait[0], longestWait[1000]) << "the nearest fine page waits less than a coarse page";
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

// A cache that plans with a set of the demand and one map lookup per tile. The equivalence test
// runs it beside VirtualShadowPageCache and requires the same rows.
class ReferencePageCache
{
public:
    explicit ReferencePageCache(uint32_t tileCount) : m_Tiles(tileCount) {}

    std::vector<VirtualShadowPageWork> Plan(std::span<const VirtualShadowPage> requests, uint64_t frame,
                                            uint64_t completedFrame)
    {
        std::set<VirtualShadowPage> demanded;
        std::vector<VirtualShadowPageWork> work;
        for (const auto& page : requests)
            if (IsValid(page) && demanded.insert(page).second)
                work.push_back({.Page = page});
        const auto residentCount = std::min(work.size(), m_Tiles.size());
        for (std::size_t index = residentCount; index < work.size(); ++index)
            demanded.erase(work[index].Page);

        std::vector<uint32_t> available;
        for (uint32_t index = 0; index < m_Tiles.size(); ++index)
        {
            const auto& tile = m_Tiles[index];
            if (!tile.Page || (tile.LastUse <= completedFrame && !demanded.contains(*tile.Page)))
                available.push_back(index);
        }
        std::ranges::sort(available, {}, [this](uint32_t index) {
            return std::tuple{m_Tiles[index].Page.has_value(), m_Tiles[index].LastUse, index};
        });
        std::size_t next = 0;
        for (auto& row : std::span(work).first(residentCount))
        {
            if (const auto found = m_Mappings.find(row.Page); found != m_Mappings.end())
                row.Tile = found->second;
            else if (next < available.size())
            {
                row.Tile = available[next++];
                auto& tile = m_Tiles[row.Tile];
                if (tile.Page)
                    m_Mappings.erase(*tile.Page);
                tile.Page = row.Page;
                Invalidate(tile);
                m_Mappings[row.Page] = row.Tile;
            }
            if (row.Tile == kUnmappedShadowPage)
                continue;
            auto& tile = m_Tiles[row.Tile];
            tile.LastUse = frame;
            row.Revision = tile.Revision;
            row.NeedsRender = !tile.Rendered;
            if (row.NeedsRender)
                tile.RenderFrame = frame;
        }
        RefreshDeformed(std::span(work).first(residentCount), frame);
        return work;
    }

    void MarkRendered(const VirtualShadowPageWork& work)
    {
        auto& tile = m_Tiles[work.Tile];
        if (work.NeedsRender && tile.Page == work.Page && tile.Revision == work.Revision)
            tile.Rendered = true;
    }
    void MarkDirty(const VirtualShadowPage& page)
    {
        if (const auto found = m_Mappings.find(page); found != m_Mappings.end())
            Invalidate(m_Tiles[found->second]);
    }
    void MarkDeformed()
    {
        for (auto& tile : m_Tiles)
            tile.Deformed = tile.Rendered;
    }

private:
    struct Tile
    {
        std::optional<VirtualShadowPage> Page;
        uint64_t LastUse = 0;
        uint64_t Revision = 0;
        uint64_t RenderFrame = 0;
        bool Rendered = false;
        bool Deformed = false;
    };

    static bool IsValid(const VirtualShadowPage& page)
    {
        if (page.Family == VirtualShadowLight::Directional)
            return page.Projection < kVirtualShadowClipLevels && page.Mip == 0;
        if ((page.Family == VirtualShadowLight::Spot && page.Projection != 0) ||
            (page.Family == VirtualShadowLight::Point && page.Projection >= 6) || page.Mip > 7 || page.X < 0 ||
            page.Y < 0)
            return false;
        const int64_t side = kVirtualLocalShadowPagesPerAxis >> page.Mip;
        return page.X < side && page.Y < side;
    }

    void Invalidate(Tile& tile)
    {
        tile.Revision = m_NextRevision++;
        tile.Rendered = false;
        tile.Deformed = false;
    }

    void RefreshDeformed(std::span<VirtualShadowPageWork> rows, uint64_t frame)
    {
        const auto missing = static_cast<uint32_t>(std::ranges::count_if(rows, &VirtualShadowPageWork::NeedsRender));
        if (missing >= kVirtualShadowPageRenderBudget)
            return;
        std::vector<std::pair<VirtualShadowPageWork*, double>> deformed;
        for (std::size_t index = 0; index < rows.size(); ++index)
        {
            auto& row = rows[index];
            if (row.Tile == kUnmappedShadowPage || row.NeedsRender || !m_Tiles[row.Tile].Deformed)
                continue;
            const uint32_t level = row.Page.Family == VirtualShadowLight::Directional ? row.Page.Projection : row.Page.Mip;
            const uint64_t age = frame - std::min(frame, m_Tiles[row.Tile].RenderFrame);
            const double limit = static_cast<double>(kVirtualShadowDeformedMaxAgeFrames << std::min(level, 16u)) *
                                 (1.0 + static_cast<double>(index) / kVirtualShadowDeformedRefreshRowScale);
            deformed.emplace_back(&row, static_cast<double>(age) / limit);
        }
        const auto count = std::min<std::size_t>(
            deformed.size(), std::min(kVirtualShadowDeformedRefreshBudget, kVirtualShadowPageRenderBudget - missing));
        std::ranges::partial_sort(deformed, deformed.begin() + static_cast<std::ptrdiff_t>(count), {},
                                  [](const auto& candidate) { return -candidate.second; });
        for (const auto& [row, staleness] : std::span(deformed).first(count))
        {
            auto& tile = m_Tiles[row->Tile];
            Invalidate(tile);
            tile.RenderFrame = frame;
            row->Revision = tile.Revision;
            row->NeedsRender = true;
            row->Refresh = true;
        }
    }

    std::vector<Tile> m_Tiles;
    std::map<VirtualShadowPage, uint32_t> m_Mappings;
    uint64_t m_NextRevision = 1;
};

// Pages of all families, a few invalid, drawn with repeats from a pool larger than the tiles.
std::vector<VirtualShadowPage> RandomDemand(std::mt19937& random)
{
    std::vector<VirtualShadowPage> demand(std::uniform_int_distribution<size_t>(0, 160)(random));
    for (auto& page : demand)
    {
        const auto family = static_cast<VirtualShadowLight>(random() % 3);
        const auto projection = static_cast<uint8_t>(random() % (family == VirtualShadowLight::Point ? 7 : 3));
        const auto mip = static_cast<uint8_t>(family == VirtualShadowLight::Directional ? random() % 5 / 4 : random() % 3);
        page = {random() % 2, family, projection, mip, static_cast<int64_t>(random() % 12) - 1,
                static_cast<int64_t>(random() % 6)};
    }
    return demand;
}

TEST(VirtualShadowPageCacheTests, PlanRowsMatchTheSetBasedReferenceOverRandomFrames)
{
    constexpr uint32_t kTiles = 48;
    VirtualShadowPageCache cache(kTiles);
    ReferencePageCache reference(kTiles);
    std::mt19937 random(11);
    size_t overflowFrames = 0;
    for (uint64_t frame = 1; frame <= 400; ++frame)
    {
        const uint64_t completed = frame - std::min<uint64_t>(frame, 1 + random() % 3);
        const auto demand = RandomDemand(random);
        const auto rows = cache.Plan(demand, frame, completed);
        const auto expected = reference.Plan(demand, frame, completed);
        ASSERT_EQ(rows.size(), expected.size()) << "frame " << frame;
        overflowFrames += rows.size() > kTiles ? 1 : 0;
        for (size_t index = 0; index < rows.size(); ++index)
        {
            const auto& row = rows[index];
            const auto& want = expected[index];
            ASSERT_EQ(std::tie(row.Page, row.Tile, row.Revision, row.NeedsRender, row.Refresh),
                      std::tie(want.Page, want.Tile, want.Revision, want.NeedsRender, want.Refresh))
                << "frame " << frame << " row " << index;
        }
        for (const auto& row : rows)
        {
            if (row.Tile == kUnmappedShadowPage || random() % 4 == 0)
                continue;
            cache.MarkRendered(row);
            reference.MarkRendered(row);
        }
        if (random() % 5 == 0)
        {
            cache.MarkDeformed();
            reference.MarkDeformed();
        }
        if (!demand.empty() && random() % 3 == 0)
        {
            const auto& page = demand[random() % demand.size()];
            cache.MarkDirty(page);
            reference.MarkDirty(page);
        }
    }
    EXPECT_GT(overflowFrames, 20u) << "the demand must often exceed the tiles";
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
