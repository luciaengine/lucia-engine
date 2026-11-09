#include "Mathematics/Triangulation.h"

#include "Mathematics/Angle.h"
#include "Mathematics/Geometry.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <set>
#include <utility>
#include <vector>

using GameEngine::Mathematics::BridgeHole;
using GameEngine::Mathematics::EarClip;
using GameEngine::Mathematics::EarClipTolerances;
using GameEngine::Mathematics::EarClipDetail::FlipAllToDelaunay;
using GameEngine::Mathematics::PointInPolygon;
using GameEngine::Mathematics::PolygonDoubledSignedArea;
using GameEngine::Mathematics::ShouldFlipForDelaunay;
using GameEngine::Mathematics::Vector2;

namespace
{

std::vector<std::uint32_t> Sequence(std::size_t count)
{
    std::vector<std::uint32_t> indices(count);
    std::iota(indices.begin(), indices.end(), 0u);
    return indices;
}

float DoubledArea(const Vector2& a, const Vector2& b, const Vector2& c)
{
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

// Every triangle wound counter-clockwise (or flat), their areas summing to the polygon's.
void ExpectCovers(const std::vector<Vector2>& points, const std::vector<std::uint32_t>& triangles,
                  float polygonDoubledArea)
{
    ASSERT_EQ(triangles.size() % 3u, 0u);
    float sum = 0.0f;
    for (std::size_t t = 0; t < triangles.size(); t += 3)
    {
        const float area = DoubledArea(points[triangles[t]], points[triangles[t + 1]], points[triangles[t + 2]]);
        EXPECT_GE(area, -1.0e-5f) << "triangle " << t / 3 << " is wound clockwise";
        sum += area;
    }
    EXPECT_NEAR(sum, polygonDoubledArea, 1.0e-4f);
}

// Every side two triangles share meets the Delaunay rule: none would flip.
void ExpectDelaunay(const std::vector<Vector2>& points, const std::vector<std::uint32_t>& triangles)
{
    // Each side (from, to) with the corner opposite it, sorted for a binary search.
    std::vector<std::array<std::uint32_t, 3>> sides;
    for (std::size_t t = 0; t < triangles.size(); t += 3)
    {
        for (std::size_t k = 0; k < 3; ++k)
            sides.push_back({triangles[t + k], triangles[t + (k + 1) % 3], triangles[t + (k + 2) % 3]});
    }
    std::sort(sides.begin(), sides.end());
    for (const auto& [from, to, c] : sides)
    {
        const auto across = std::lower_bound(sides.begin(), sides.end(), std::array<std::uint32_t, 3>{to, from, 0u});
        if (across == sides.end() || (*across)[0] != to || (*across)[1] != from)
            continue;
        EXPECT_FALSE(ShouldFlipForDelaunay(points[from], points[to], points[c], points[(*across)[2]], 1.0e-12f))
            << "side " << from << "-" << to << " breaks the Delaunay rule";
    }
}

// A 20 x 20 square with a road 4 wide running up from its top edge, each corner where they meet
// rounded by a quarter arc of radius 1.5, a point every 10 degrees.
std::vector<Vector2> SquareWithRoundedRoad()
{
    std::vector<Vector2> shape{{-10.0f, -20.0f}, {10.0f, -20.0f}, {10.0f, 0.0f}};
    for (int step = 0; step <= 9; ++step)
    {
        const float angle = GameEngine::Mathematics::ToRadians(270.0f - 10.0f * static_cast<float>(step));
        shape.emplace_back(3.5f + 1.5f * std::cos(angle), 1.5f + 1.5f * std::sin(angle));
    }
    shape.emplace_back(2.0f, 20.0f);
    shape.emplace_back(-2.0f, 20.0f);
    for (int step = 0; step <= 9; ++step)
    {
        const float angle = GameEngine::Mathematics::ToRadians(360.0f - 10.0f * static_cast<float>(step));
        shape.emplace_back(-3.5f + 1.5f * std::cos(angle), 1.5f + 1.5f * std::sin(angle));
    }
    shape.emplace_back(-10.0f, 0.0f);
    return shape;
}

// A band 0.5 wide between radii 20 and 20.5 over a quarter turn, each arc a point every 3 degrees.
std::vector<Vector2> ThinCurvedBand()
{
    std::vector<Vector2> shape;
    for (int step = 0; step <= 30; ++step)
    {
        const float angle = GameEngine::Mathematics::ToRadians(3.0f * static_cast<float>(step));
        shape.emplace_back(20.5f * std::cos(angle), 20.5f * std::sin(angle));
    }
    for (int step = 30; step >= 0; --step)
    {
        const float angle = GameEngine::Mathematics::ToRadians(3.0f * static_cast<float>(step));
        shape.emplace_back(20.0f * std::cos(angle), 20.0f * std::sin(angle));
    }
    return shape;
}

// `count` corners on a circle of `radius` around the origin, counter-clockwise.
std::vector<Vector2> RegularPolygon(std::size_t count, float radius)
{
    std::vector<Vector2> shape;
    for (std::size_t i = 0; i < count; ++i)
    {
        const float turn = static_cast<float>(i) / static_cast<float>(count);
        const float angle = GameEngine::Mathematics::ToRadians(360.0f * turn);
        shape.emplace_back(radius * std::cos(angle), radius * std::sin(angle));
    }
    return shape;
}

// The seconds that `run` takes on the steady clock.
template <typename Run>
double SecondsFor(Run run)
{
    const auto start = std::chrono::steady_clock::now();
    run();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

} // namespace

// The triangles of an outline with runs of close corners (a road's rounded corners, a thin curved
// band) meet the Delaunay rule, so the corners of an arc do not fan thin triangles across it.
TEST(EarClip, TheTrianglesOfOutlinesWithArcsMeetTheDelaunayRule)
{
    for (const std::vector<Vector2>& shape : {SquareWithRoundedRoad(), ThinCurvedBand()})
    {
        SCOPED_TRACE(::testing::Message() << shape.size() << " corners");
        std::vector<std::uint32_t> triangles;
        EarClip(shape, Sequence(shape.size()), triangles);
        ASSERT_EQ(triangles.size(), 3u * (shape.size() - 2u));
        ExpectCovers(shape, triangles, PolygonDoubledSignedArea(shape));
        ExpectDelaunay(shape, triangles);
    }
}

// A rectangle's two ear-clipped triangles already meet the rule (its corners lie on one circle):
// no side flips.
TEST(EarClip, ARectanglesTrianglesStayAsClipped)
{
    const std::vector<Vector2> rectangle{{0, 0}, {4, 0}, {4, 1}, {0, 1}};
    std::vector<std::uint32_t> triangles;
    EarClip(rectangle, Sequence(rectangle.size()), triangles);
    EXPECT_EQ(triangles, (std::vector<std::uint32_t>{3, 0, 1, 1, 2, 3}));
}

// Points on one circle make every flip test a near tie: the flips end, and quickly, for the
// corners of a 256-gon of radius 1 and for a fan over a 10,000-gon.
TEST(EarClip, TheFlipsEndForCornersOnOneCircle)
{
    constexpr double kMaxSeconds = 2.0;
    const std::vector<Vector2> polygon = RegularPolygon(256, 1.0f);
    std::vector<std::uint32_t> triangles;
    EXPECT_LT(SecondsFor([&] { EarClip(polygon, Sequence(polygon.size()), triangles); }), kMaxSeconds);
    ASSERT_EQ(triangles.size(), 3u * (polygon.size() - 2u));
    ExpectCovers(polygon, triangles, PolygonDoubledSignedArea(polygon));
    ExpectDelaunay(polygon, triangles);

    const std::vector<Vector2> large = RegularPolygon(10000, 1.0f);
    std::vector<std::uint32_t> fan;
    for (std::uint32_t i = 1; i + 1 < large.size(); ++i)
        fan.insert(fan.end(), {0u, i, i + 1u});
    EXPECT_LT(SecondsFor([&] { FlipAllToDelaunay(large, fan, EarClipTolerances{}.MinDoubledArea); }), kMaxSeconds);
    ExpectDelaunay(large, fan);
}

// The rule flips a side only when both triangles after the flip are wider than the minimum: a quad
// whose corners c and d lie close to the side ab flips by its angles, but each new triangle has a
// doubled area of 0.2.
TEST(EarClip, TheFlipRuleRefusesNewTrianglesBelowTheMinimumArea)
{
    const Vector2 a{-1.0f, 0.0f}, b{1.0f, 0.0f}, c{0.0f, 0.1f}, d{0.0f, -0.1f};
    EXPECT_TRUE(ShouldFlipForDelaunay(a, b, c, d, 0.1f));
    EXPECT_FALSE(ShouldFlipForDelaunay(a, b, c, d, 0.5f));
}

TEST(EarClip, AConvexQuadIsTwoCounterClockwiseTriangles)
{
    const std::vector<Vector2> square{{0, 0}, {2, 0}, {2, 2}, {0, 2}};
    std::vector<std::uint32_t> triangles;
    EarClip(square, Sequence(square.size()), triangles);
    EXPECT_EQ(triangles.size(), 6u);
    ExpectCovers(square, triangles, PolygonDoubledSignedArea(square));
}

TEST(EarClip, AConcaveOutlineKeepsEveryTriangleInside)
{
    // An L: the reflex corner at (1, 1) must not be cut across.
    const std::vector<Vector2> shape{{0, 0}, {3, 0}, {3, 1}, {1, 1}, {1, 3}, {0, 3}};
    std::vector<std::uint32_t> triangles;
    EarClip(shape, Sequence(shape.size()), triangles);
    ASSERT_EQ(triangles.size(), 3u * (shape.size() - 2u));
    ExpectCovers(shape, triangles, PolygonDoubledSignedArea(shape));
    for (std::size_t t = 0; t < triangles.size(); t += 3)
    {
        const Vector2 centroid = (shape[triangles[t]] + shape[triangles[t + 1]] + shape[triangles[t + 2]]) / 3.0f;
        EXPECT_TRUE(PointInPolygon(centroid, shape)) << "triangle " << t / 3 << " lies outside the L";
    }
}

TEST(EarClip, AStraightCornerStaysAnEdgeOfTheTriangles)
{
    // The mesh plane cut leaves a straight-on corner on every face it crosses; dropping it would
    // open the cap against the faces around it.
    // The last three corners left run straight along the top edge.
    const std::vector<Vector2> shape{{0, 0}, {2, 0}, {2, 1}, {1, 1}, {0, 1}};
    std::vector<std::uint32_t> triangles;
    EarClip(shape, Sequence(shape.size()), triangles);
    ASSERT_EQ(triangles.size(), 9u);
    std::set<std::pair<std::uint32_t, std::uint32_t>> edges;
    for (std::size_t t = 0; t < triangles.size(); t += 3)
    {
        for (std::size_t k = 0; k < 3; ++k)
            edges.insert(std::minmax(triangles[t + k], triangles[t + (k + 1) % 3]));
    }
    for (std::uint32_t i = 0; i < shape.size(); ++i)
        EXPECT_TRUE(edges.count(std::minmax(i, static_cast<std::uint32_t>((i + 1) % shape.size()))))
            << "outline edge " << i << " is not an edge of any triangle";
    ExpectCovers(shape, triangles, PolygonDoubledSignedArea(shape));
}

TEST(EarClip, AHoleBridgedIntoTheOutlineIsLeftUncovered)
{
    // The plane cut's capped tube: a 4 x 4 square with a 2 x 2 hole, the clockwise hole spliced in
    // from its rightmost corner to the outline's (the bridge runs both ways along one edge).
    const std::vector<Vector2> points{{0, 0}, {4, 0}, {4, 4}, {0, 4}, {3, 1}, {1, 1}, {1, 3}, {3, 3}};
    const std::vector<std::uint32_t> bridged{0, 1, 2, 7, 4, 5, 6, 7, 2, 3};
    std::vector<std::uint32_t> triangles;
    EarClip(points, bridged, triangles);
    ExpectCovers(points, triangles, 2.0f * (16.0f - 4.0f));
    for (std::size_t t = 0; t < triangles.size(); t += 3)
    {
        const Vector2 centroid = (points[triangles[t]] + points[triangles[t + 1]] + points[triangles[t + 2]]) / 3.0f;
        EXPECT_FALSE(centroid.x > 1.0f && centroid.x < 3.0f && centroid.y > 1.0f && centroid.y < 3.0f)
            << "triangle " << t / 3 << " covers the hole";
    }
}

TEST(EarClip, AFoldedOutlineEndsWithoutHanging)
{
    // A figure eight has no area to cover consistently; the clip must still end, with n - 2
    // triangles.
    const std::vector<Vector2> eight{{0, 0}, {2, 2}, {2, 0}, {0, 2}};
    std::vector<std::uint32_t> triangles;
    EarClip(eight, Sequence(eight.size()), triangles);
    EXPECT_EQ(triangles.size(), 6u);
}

TEST(EarClip, ASlitLeftWithFlatCornersOnlyIsClippedAtAFlatCorner)
{
    // A zero-width slit, as a bridge spliced in for a hole leaves: the outline runs from (4, 3)
    // down to (2, 1), back up to (3, 2), and later along the slit's line again from (4, 2) to
    // (0, 0). Once its true ears are clipped only flat corners remain, and the next ear is one of
    // them; any other corner would cut a triangle wound clockwise, across the outline.
    const std::vector<Vector2> slit{{4, 3}, {2, 1}, {3, 2}, {4, 2}, {0, 0}, {4, 0}, {4, 4}};
    std::vector<std::uint32_t> triangles;
    EarClip(slit, Sequence(slit.size()), triangles);
    EXPECT_EQ(triangles.size(), 3u * (slit.size() - 2u));
    ExpectCovers(slit, triangles, PolygonDoubledSignedArea(slit));
}

// A square with a square hole: the hole bridged in, the triangles cover the square less the hole
// and none lies over the hole.
TEST(EarClip, AnOutlineWithABridgedHoleLeavesTheHoleUncovered)
{
    const std::vector<Vector2> points{{0, 0}, {10, 0}, {10, 10}, {0, 10}, {4, 4}, {4, 6}, {6, 6}, {6, 4}};
    std::vector<std::uint32_t> outline{0, 1, 2, 3};
    const std::vector<std::uint32_t> hole{4, 5, 6, 7}; // clockwise
    BridgeHole(points, outline, hole);
    EXPECT_EQ(outline.size(), 4u + 4u + 2u) << "the hole and the bridge's two repeated corners";
    std::vector<std::uint32_t> triangles;
    EarClip(points, std::move(outline), triangles);
    ExpectCovers(points, triangles, 2.0f * (100.0f - 4.0f));
    for (std::size_t t = 0; t < triangles.size(); t += 3)
    {
        const Vector2 centroid = (points[triangles[t]] + points[triangles[t + 1]] + points[triangles[t + 2]]) * (1.0f / 3.0f);
        EXPECT_FALSE(centroid.x > 4.0f && centroid.x < 6.0f && centroid.y > 4.0f && centroid.y < 6.0f)
            << "triangle " << t / 3 << " covers the hole";
    }
}

// Two bands joined by a rung (an H on its side): once the clip has cut away one band's far end,
// the diagonal of the next ear runs along that band's inner side, through the corner where the
// rung meets it. That corner on the diagonal blocks the ear, so no triangle crosses the notch.
TEST(EarClip, ACornerOnAnEarsDiagonalBlocksTheEar)
{
    const std::vector<Vector2> h{{2, -2}, {10, -2}, {10, -6.5f}, {14, -6.5f}, {14, 6.5f}, {10, 6.5f},
                                 {10, 2}, {2, 2},   {2, 6.5f},   {-2, 6.5f},  {-2, -6.5f}, {2, -6.5f}};
    std::vector<std::uint32_t> triangles;
    EarClip(h, Sequence(h.size()), triangles);
    EXPECT_EQ(triangles.size(), 3u * (h.size() - 2u));
    ExpectCovers(h, triangles, PolygonDoubledSignedArea(h));
    for (std::size_t t = 0; t < triangles.size(); t += 3)
    {
        const Vector2 centroid = (h[triangles[t]] + h[triangles[t + 1]] + h[triangles[t + 2]]) * (1.0f / 3.0f);
        EXPECT_TRUE(PointInPolygon(centroid, h)) << "triangle " << t / 3 << " lies outside the outline";
    }
}
