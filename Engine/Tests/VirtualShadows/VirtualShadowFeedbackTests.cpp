#include "Engine/Rendering/VirtualShadows/VirtualShadowFeedback.h"

#include "Engine/Rendering/ShadowCasterChanges.h"
#include "Mathematics/MatrixOps.h"
#include "Rendering/Common/Frustum.h"

#include <gtest/gtest.h>

#include <array>

namespace GameEngine::Engine::Renderer
{
namespace
{

const VirtualShadowFeedbackProjection kSun{
    {8, VirtualShadowLight::Directional, 2, 0, -19, 13}, Mathematics::Matrix4x4::Identity(), 0, 2, 0};
const VirtualShadowFeedbackProjection kSpot{
    {8, VirtualShadowLight::Spot, 0, 0, 0, 0}, Mathematics::Matrix4x4::Identity(), 30, 2, 1,
    std::nullopt, Mathematics::WorldPosition{}};
const VirtualShadowFeedbackProjection kPoint{
    {8, VirtualShadowLight::Point, 4, 0, 0, 0}, Mathematics::Matrix4x4::Identity(), 35, 2, 1,
    std::nullopt, Mathematics::WorldPosition{}};

TEST(VirtualShadowFeedbackTests, LocalLightMotionBelowWorldMatrixPrecisionRejectsDelayedFeedback)
{
    for (const auto& source : {kSpot, kPoint})
    {
        auto before = source;
        before.LocalPosition = Mathematics::WorldPosition{409600000.0, 0.0, 0.0};
        auto after = before;
        after.LocalPosition->X += 0.125;
        EXPECT_FALSE(SameVirtualShadowProjection(before, after));

        std::array<uint32_t, 2> words{};
        words[before.FirstPage / 32] = uint32_t{1} << (before.FirstPage % 32);
        const VirtualShadowFeedbackLayout dispatched{19, 3, 2, {before}};
        const VirtualShadowFeedbackLayout current{19, 3, 2, {after}};
        EXPECT_TRUE(DecodeVirtualShadowFeedback(words, dispatched, current).empty());
    }
}

TEST(VirtualShadowFeedbackTests, DelayedBitsRetainWorldCoordinatesAcrossProjectionReorder)
{
    const VirtualShadowFeedbackLayout submitted{19, 3, 2, {kSun, kSpot, kPoint}};
    auto active = submitted;
    active.Projections = {kPoint, kSun, kSpot};
    active.Projections[0].FirstPage = 0;
    active.Projections[1].FirstPage = 5;
    active.Projections[2].FirstPage = 9;

    constexpr uint32_t kWord0 = (1u << 1) | (1u << 2) | (1u << 31);
    constexpr uint32_t kWord1 = (1u << (33 - 32)) | (1u << (34 - 32)) |
                                 (1u << (37 - 32)) | (1u << (39 - 32));
    const std::vector<VirtualShadowPage> expected{
        {8, VirtualShadowLight::Point, 4, 1, 0, 0},
        {8, VirtualShadowLight::Spot, 0, 1, 0, 0},
        {8, VirtualShadowLight::Point, 4, 0, 0, 1},
        {8, VirtualShadowLight::Directional, 2, 0, -18, 13},
        {8, VirtualShadowLight::Spot, 0, 0, 1, 0},
        {8, VirtualShadowLight::Directional, 2, 0, -19, 14},
        {8, VirtualShadowLight::Spot, 0, 0, 1, 1},
    };
    EXPECT_EQ(DecodeVirtualShadowFeedback(std::array{kWord0, kWord1}, submitted, active), expected);
    EXPECT_EQ(FindVirtualShadowFeedbackBit(active.Projections[0], expected[0]), 4u);
    EXPECT_EQ(FindVirtualShadowFeedbackBit(active.Projections[1], expected[3]), 6u);
    EXPECT_EQ(FindVirtualShadowFeedbackBit(active.Projections[2], expected[6]), 12u);
    EXPECT_FALSE(FindVirtualShadowFeedbackBit(active.Projections[0], expected[3]));
}

TEST(VirtualShadowFeedbackTests, RejectsChangedGeometryAndAnEarlierWorldOrLayerMask)
{
    const VirtualShadowFeedbackLayout submitted{19, 3, 2, {kSun, kSpot, kPoint}};
    auto active = submitted;
    active.Projections[1].WorldToLight[3][0] = 1.0f;
    ++active.Projections[2].BasePage.Projection;
    constexpr std::array words{uint32_t{1} | (1u << 30), 1u << (35 - 32)};
    const std::vector expected{kSun.BasePage};
    EXPECT_EQ(DecodeVirtualShadowFeedback(words, submitted, active), expected);

    ++active.World;
    EXPECT_TRUE(DecodeVirtualShadowFeedback(words, submitted, active).empty());
    active.World = submitted.World;
    active.RenderLayerMask = 1;
    EXPECT_TRUE(DecodeVirtualShadowFeedback(words, submitted, active).empty());
}

TEST(VirtualShadowFeedbackTests, InvalidBitRangeDoesNotReadTheOtherProjectionOrConsumeItsBudget)
{
    auto outOfRange = kPoint;
    outOfRange.FirstPage = 63;
    const VirtualShadowFeedbackLayout layout{19, 3, 2, {outOfRange, kSun}};
    constexpr std::array words{uint32_t{1}, 1u << 31};
    const std::vector expected{kSun.BasePage};
    EXPECT_EQ(DecodeVirtualShadowFeedback(words, layout, layout), expected);
    EXPECT_TRUE(DecodeVirtualShadowFeedback(std::span(words).first(1), layout, layout).empty());
}

TEST(VirtualShadowFeedbackTests, MovedCasterDirtiesDepartureAndArrivalPagesWithoutTouchingAnUnrelatedPage)
{
    const VirtualShadowFeedbackProjection projection{
        {3, VirtualShadowLight::Spot}, Mathematics::Matrix4x4::PerspectiveReverseZ(
            1.57079632679f, 1.0f, 0.05f, 20.0f), 0, 4, 2,
        std::nullopt, Mathematics::WorldPosition{}};
    constexpr VirtualShadowPage left{3, VirtualShadowLight::Spot, 0, 0, 0, 1};
    constexpr VirtualShadowPage right{3, VirtualShadowLight::Spot, 0, 0, 3, 1};
    constexpr VirtualShadowPage unrelated{3, VirtualShadowLight::Spot, 0, 0, 0, 3};
    constexpr std::array changes{
        ShadowCasterChangeSphere{-1.5f, .5f, 2.0f, .1f},
        ShadowCasterChangeSphere{1.5f, .5f, 2.0f, .1f},
    };
    EXPECT_TRUE(VirtualShadowPageTouchedByCasters(projection, left, std::span(changes).first(1)));
    EXPECT_FALSE(VirtualShadowPageTouchedByCasters(projection, left, std::span(changes).last(1)));
    EXPECT_TRUE(VirtualShadowPageTouchedByCasters(projection, right, std::span(changes).last(1)));
    EXPECT_FALSE(VirtualShadowPageTouchedByCasters(projection, unrelated, changes));

    VirtualShadowPageCache cache(3);
    constexpr std::array pages{left, right, unrelated};
    for (const auto& row : cache.Plan(pages, 1, 0))
        ASSERT_TRUE(cache.MarkRendered(row));
    cache.InvalidatePages([&](const auto& page) { return VirtualShadowPageTouchedByCasters(projection, page, changes); });
    const auto refresh = cache.Plan(pages, 2, 1);
    ASSERT_EQ(refresh.size(), 3u);
    EXPECT_TRUE(refresh[0].NeedsRender);
    EXPECT_TRUE(refresh[1].NeedsRender);
    EXPECT_FALSE(refresh[2].NeedsRender);
}

// A caster writes a page only inside the page's side planes. The sun raster clamps near depth, so
// its page is open toward the light. A spot page ends at the light.
TEST(VirtualShadowFeedbackTests, PageCasterPlanesBoundThePageAndOpenOnlyTheSunTowardTheLight)
{
    const auto sun = CropVirtualShadowProjection(
        Mathematics::MakeOrthographicLH_ZO_ReverseZ(-4.0f, 4.0f, -4.0f, 4.0f, 1.0f, 100.0f), 2, 0, 0);
    const auto spot = CropVirtualShadowProjection(Mathematics::Matrix4x4::PerspectiveReverseZ(1.2f, 1.0f, .5f, 100.0f), 2, 0, 0);
    ASSERT_TRUE(sun && spot);
    const auto sunPlanes = VirtualShadowPageCasterPlanes(*sun, VirtualShadowLight::Directional);
    const auto spotPlanes = VirtualShadowPageCasterPlanes(*spot, VirtualShadowLight::Spot);
    const auto inside = [](const auto& planes, Mathematics::Vector3 center)
    { return Rendering::TestSphereFrustum(center, .05f, planes.data()); };

    EXPECT_TRUE(inside(sunPlanes, {-2.0f, 2.0f, 10.0f}));
    EXPECT_TRUE(inside(sunPlanes, {0.0f, 2.0f, 10.0f}));
    EXPECT_FALSE(inside(sunPlanes, {2.0f, 2.0f, 10.0f}));
    EXPECT_FALSE(inside(sunPlanes, {-2.0f, -2.0f, 10.0f}));
    EXPECT_TRUE(inside(sunPlanes, {-2.0f, 2.0f, -50.0f}));

    EXPECT_TRUE(inside(spotPlanes, {-2.0f, 2.0f, 10.0f}));
    EXPECT_TRUE(inside(spotPlanes, {0.0f, 2.0f, 10.0f}));
    EXPECT_FALSE(inside(spotPlanes, {2.0f, 2.0f, 10.0f}));
    EXPECT_FALSE(inside(spotPlanes, {-2.0f, 2.0f, -10.0f}));
}

TEST(VirtualShadowFeedbackTests, DirectionalWindowRetainsOnlyOverlappingDelayedWorldPages)
{
    const auto rotation = Mathematics::Matrix4x4::Identity();
    constexpr Mathematics::Vector3Int kSector{};
    const auto first = BuildVirtualShadowDirectionalWindow(8, 0, 4, 4, 32.0f, rotation,
                                                            Mathematics::WorldPosition{0.1, -0.1, 1.0}, kSector);
    const auto subpage = BuildVirtualShadowDirectionalWindow(8, 0, 4, 4, 32.0f, rotation,
                                                              Mathematics::WorldPosition{1.9, -1.9, 1.0}, kSector);
    const auto next = BuildVirtualShadowDirectionalWindow(8, 0, 4, 4, 32.0f, rotation,
                                                           Mathematics::WorldPosition{2.1, -2.1, 1.0}, kSector);
    ASSERT_TRUE(first && subpage && next);
    EXPECT_EQ(first->Origin, subpage->Origin);
    EXPECT_EQ(first->Origin.X + 1, next->Origin.X);
    EXPECT_EQ(first->Origin.Y + 1, next->Origin.Y);
    EXPECT_DOUBLE_EQ(first->Grid.PageWorldSize, 2.0);

    const VirtualShadowFeedbackProjection oldRow{first->Origin, first->WorldToLight, 0, 4, 0, first->Grid};
    const VirtualShadowFeedbackProjection newRow{next->Origin, next->WorldToLight, 8, 4, 0, next->Grid};
    EXPECT_TRUE(SameVirtualShadowProjection(oldRow, newRow));
    const VirtualShadowFeedbackLayout submitted{19, 3, 1, {oldRow}};
    const VirtualShadowFeedbackLayout active{19, 3, 1, {newRow}};
    const auto overlap = next->Origin;
    constexpr std::array bits{(1u << 0) | (1u << 5)};
    EXPECT_EQ(DecodeVirtualShadowFeedback(bits, submitted, active), std::vector{overlap});
    EXPECT_EQ(FindVirtualShadowFeedbackBit(newRow, overlap), 8u);
    EXPECT_FALSE(FindVirtualShadowFeedbackBit(newRow, first->Origin));

    const auto oldPage = VirtualShadowDirectionalPageMatrix(first->Grid, overlap, kSector);
    const auto newPage = VirtualShadowDirectionalPageMatrix(next->Grid, overlap, kSector);
    EXPECT_EQ(oldPage[3][0], newPage[3][0]);
    EXPECT_EQ(oldPage[3][1], newPage[3][1]);
    EXPECT_EQ(oldPage[3][2], newPage[3][2]);
    // The page center at (-1, +1) maps to (0, 0) in its raster viewport.
    EXPECT_FLOAT_EQ(-oldPage[0][0] + oldPage[3][0], 0.0f);
    EXPECT_FLOAT_EQ(oldPage[1][1] + oldPage[3][1], 0.0f);

    constexpr std::array changes{ShadowCasterChangeSphere{-3.0f, 3.0f, 0.0f, 0.1f}};
    EXPECT_TRUE(VirtualShadowPageTouchedByCasters(newRow, first->Origin, changes));
    EXPECT_FALSE(VirtualShadowPageTouchedByCasters(newRow, overlap, changes));

    auto changed = active;
    ++changed.Projections[0].DirectionalGrid->DepthBand;
    EXPECT_FALSE(SameVirtualShadowProjection(oldRow, changed.Projections[0]));
    EXPECT_TRUE(DecodeVirtualShadowFeedback(bits, submitted, changed).empty());
    changed = active;
    changed.Projections[0].DirectionalGrid->LightAxes[0][0] = -1.0f;
    EXPECT_FALSE(SameVirtualShadowProjection(oldRow, changed.Projections[0]));
}

// The physical pool is smaller than the demand of a view. The pages nearest to the window
// center are nearest to the camera, so they must get tiles first.
TEST(VirtualShadowFeedbackTests, DirectionalDemandStartsAtTheWindowCenter)
{
    const auto window = BuildVirtualShadowDirectionalWindow(8, 0, 4, 4, 32.0f, Mathematics::Matrix4x4::Identity(),
                                                             Mathematics::WorldPosition{0.1, -0.1, 1.0}, {});
    ASSERT_TRUE(window);
    const VirtualShadowFeedbackProjection row{window->Origin, window->WorldToLight, 0, 4, 0, window->Grid};
    const VirtualShadowFeedbackLayout layout{19, 3, 1, {row}};
    constexpr std::array kAllPages{0xFFFFu};
    const auto demand = DecodeVirtualShadowFeedback(kAllPages, layout, layout);
    ASSERT_EQ(demand.size(), 16u);
    const auto ring = [&window](const VirtualShadowPage& page)
    {
        const int64_t x = 2 * (page.X - window->Origin.X) - 3;
        const int64_t y = 2 * (page.Y - window->Origin.Y) - 3;
        return x * x + y * y;
    };
    for (size_t index = 0; index < 4; ++index)
        EXPECT_EQ(ring(demand[index]), 2) << index;
    for (size_t index = 4; index < 12; ++index)
        EXPECT_EQ(ring(demand[index]), 10) << index;
    for (size_t index = 12; index < 16; ++index)
        EXPECT_EQ(ring(demand[index]), 18) << index;
}

TEST(VirtualShadowFeedbackTests, DirectionalPageProjectionStaysPreciseAtAPlanetaryRenderOrigin)
{
    constexpr int32_t kSectorX = 400000;
    constexpr double kOriginX = static_cast<double>(kSectorX) * Mathematics::kWorldSectorSize;
    const auto rotation = Mathematics::Matrix4x4::Identity();
    constexpr Mathematics::Vector3Int kSector{kSectorX, 0, 0};
    const auto window = BuildVirtualShadowDirectionalWindow(8, 0, 4, 4, 32.0f, rotation,
                                                             Mathematics::WorldPosition{kOriginX, 0.0, 1.0}, kSector);
    ASSERT_TRUE(window);
    EXPECT_FLOAT_EQ(window->RelativeToLight[3][0], 0.0f);
    VirtualShadowPage page = window->Origin;
    page.X += 2;
    page.Y += 2;
    EXPECT_EQ(page.X, static_cast<int64_t>(kOriginX) / 2);
    const auto matrix = VirtualShadowDirectionalPageMatrix(window->Grid, page, kSector);
    EXPECT_FLOAT_EQ(matrix[0][0] + matrix[3][0], 0.0f);
    EXPECT_FLOAT_EQ(-matrix[1][1] + matrix[3][1], 0.0f);
    EXPECT_FLOAT_EQ(matrix[3][2], 0.5f);
    EXPECT_LT(matrix[2][2], 0.0f);
}

} // namespace
} // namespace GameEngine::Engine::Renderer
