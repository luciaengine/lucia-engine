// A road's strip around the paved regions on it: the arc length it skips, the Open edge it caps at,
// and a closed road whose kept run crosses its seam; a run eases its crown flat at an Open edge.
#include "SplineRecipes/Paving/PavingRoadTrim.h"

#include "Mathematics/Interpolation.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace GameEngine::SplineRecipes
{
namespace
{

namespace SG = GameEngine::SplineGeometry;
using Mathematics::Vector3;

constexpr float32 kTolerance = 1.0e-4f;

// A road along +Z from z = 0, one station per meter, its arc length equal to z.
struct StraightRoad
{
    std::vector<SG::SplineStripStation> Stations;
    std::vector<float32> RoadDistance;
};

StraightRoad MakeStraightRoad(uint32 meters)
{
    StraightRoad road;
    for (uint32 i = 0; i <= meters; ++i)
    {
        SG::SplineStripStation station;
        station.Position = Vector3(0.0f, 0.0f, static_cast<float32>(i));
        station.Distance = static_cast<float32>(i);
        road.Stations.push_back(station);
        road.RoadDistance.push_back(static_cast<float32>(i));
    }
    return road;
}

Vector3 LeftEdge(const SG::SplineStripStation& station)
{
    return station.Position - station.Right * station.HalfWidthLeft;
}

Vector3 RightEdge(const SG::SplineStripStation& station)
{
    return station.Position + station.Right * station.HalfWidthRight;
}

void ExpectNear(const Vector3& actual, const Vector3& expected)
{
    EXPECT_NEAR(actual.x, expected.x, kTolerance);
    EXPECT_NEAR(actual.y, expected.y, kTolerance);
    EXPECT_NEAR(actual.z, expected.z, kTolerance);
}

TEST(PavingRoadTrim, ARoadThroughARegionSkipsBothArmsAndCapsAtEachOpenEdge)
{
    const StraightRoad road = MakeStraightRoad(40);
    // The node at 20; one arm out along +Z to 26, one back along -Z to 14. Travelling -Z, +X is left.
    PavingRoadTrim ahead;
    ahead.NodeDistance = 20.0f;
    ahead.StopDistance = 26.0f;
    ahead.Open = true;
    ahead.OpenLeft = Vector3(-1.0f, 0.5f, 26.0f);
    ahead.OpenRight = Vector3(1.0f, 0.7f, 26.0f);
    PavingRoadTrim behind;
    behind.NodeDistance = 20.0f;
    behind.StopDistance = 14.0f;
    behind.Open = true;
    behind.OpenLeft = Vector3(1.0f, 0.3f, 14.0f);
    behind.OpenRight = Vector3(-1.0f, 0.2f, 14.0f);
    const std::vector<PavingRoadTrim> trims = {ahead, behind};

    const TrimmedSweepStations trimmed =
        TrimSweepStations(road.Stations, road.RoadDistance, 0.0f, trims, Mathematics::Matrix4x4::Identity());

    ASSERT_EQ(trimmed.Runs.size(), 2u);
    ASSERT_EQ(trimmed.Source.size(), trimmed.Stations.size());
    for (const SG::SplineStripStation& station : trimmed.Stations)
        EXPECT_FALSE(station.Position.z > 14.0f + kTolerance && station.Position.z < 26.0f - kTolerance)
            << "a station at z " << station.Position.z << " lies under the region";

    // The strip's last ring before the region and its first ring after lie on the Open edges, at
    // the region's heights, the strip's right edge on +X.
    const SG::SplineStripStation& before = trimmed.Stations[trimmed.Runs[0].second];
    ExpectNear(LeftEdge(before), Vector3(-1.0f, 0.2f, 14.0f));
    ExpectNear(RightEdge(before), Vector3(1.0f, 0.3f, 14.0f));
    EXPECT_NEAR(before.Distance, 14.0f, kTolerance);
    const SG::SplineStripStation& after = trimmed.Stations[trimmed.Runs[1].first];
    ExpectNear(LeftEdge(after), Vector3(-1.0f, 0.5f, 26.0f));
    ExpectNear(RightEdge(after), Vector3(1.0f, 0.7f, 26.0f));
    EXPECT_NEAR(after.Distance, 26.0f, kTolerance);
    EXPECT_GT(after.Up.y, 0.99f);
    EXPECT_NEAR(Vector3::Dot(after.Forward, after.Right), 0.0f, kTolerance);

    EXPECT_NEAR(trimmed.Stations[trimmed.Runs[0].first].Position.z, 0.0f, kTolerance);
    EXPECT_NEAR(trimmed.Stations[trimmed.Runs[1].second].Position.z, 40.0f, kTolerance);
}

TEST(PavingRoadTrim, AnOpenCapKeepsTheStripsHalfWidthsAndLiesAcrossTheOpenEdge)
{
    StraightRoad road = MakeStraightRoad(40);
    for (SG::SplineStripStation& station : road.Stations)
    {
        station.HalfWidthLeft = 3.0f;
        station.HalfWidthRight = 3.0f;
    }
    // The Open edge is the top of the strip, narrower than the station's half-width.
    PavingRoadTrim ahead;
    ahead.NodeDistance = 0.0f;
    ahead.StopDistance = 26.0f;
    ahead.Open = true;
    ahead.OpenLeft = Vector3(-2.55f, 0.5f, 26.0f);
    ahead.OpenRight = Vector3(2.55f, 0.5f, 26.0f);
    const std::vector<PavingRoadTrim> trims = {ahead};

    const TrimmedSweepStations trimmed =
        TrimSweepStations(road.Stations, road.RoadDistance, 0.0f, trims, Mathematics::Matrix4x4::Identity());

    ASSERT_EQ(trimmed.Runs.size(), 1u);
    const SG::SplineStripStation& cap = trimmed.Stations[trimmed.Runs[0].first];
    ExpectNear(cap.Position, Vector3(0.0f, 0.5f, 26.0f));
    ExpectNear(cap.Right, Vector3(1.0f, 0.0f, 0.0f));
    EXPECT_NEAR(cap.HalfWidthLeft, 3.0f, kTolerance);
    EXPECT_NEAR(cap.HalfWidthRight, 3.0f, kTolerance);
}

// A run's crown eases from flat on an Open edge to whole one road width from it, by a smooth step, and
// a run with no Open end keeps its crown.
TEST(PavingRoadTrim, AnOpenCapEasesTheCrownToFlatOverOneRoadWidth)
{
    StraightRoad road = MakeStraightRoad(40);
    for (SG::SplineStripStation& station : road.Stations)
    {
        station.HalfWidthLeft = 2.0f;
        station.HalfWidthRight = 2.0f;
    }
    PavingRoadTrim ahead;
    ahead.NodeDistance = 20.0f;
    ahead.StopDistance = 26.0f;
    ahead.Open = true;
    ahead.OpenLeft = Vector3(-2.0f, 0.0f, 26.0f);
    ahead.OpenRight = Vector3(2.0f, 0.0f, 26.0f);
    PavingRoadTrim behind = ahead;
    behind.StopDistance = 14.0f;
    behind.OpenLeft = Vector3(2.0f, 0.0f, 14.0f);
    behind.OpenRight = Vector3(-2.0f, 0.0f, 14.0f);
    const std::vector<PavingRoadTrim> trims = {ahead, behind};

    const TrimmedSweepStations trimmed =
        TrimSweepStations(road.Stations, road.RoadDistance, 0.0f, trims, Mathematics::Matrix4x4::Identity());

    ASSERT_EQ(trimmed.Runs.size(), 2u);
    for (const SG::SplineStripStation& station : trimmed.Stations)
    {
        const float32 away = std::min(std::abs(station.Distance - 14.0f), std::abs(station.Distance - 26.0f));
        const float32 t = std::min(away / 4.0f, 1.0f);
        EXPECT_NEAR(station.RiseScale, t * t * (3.0f - 2.0f * t), kTolerance) << "at " << station.Distance;
    }
    EXPECT_NEAR(trimmed.Stations[trimmed.Runs[0].second].RiseScale, 0.0f, kTolerance);
    EXPECT_NEAR(trimmed.Stations[trimmed.Runs[1].first].RiseScale, 0.0f, kTolerance);
    EXPECT_NEAR(trimmed.Stations[trimmed.Runs[0].first].RiseScale, 1.0f, kTolerance);
}

// A run 2 m long between two Open caps on a 4 m road eases from each cap over one road width, so its
// crown stays low along the whole run.
TEST(PavingRoadTrim, AShortRunBetweenTwoOpenCapsEasesFromEachOverOneRoadWidth)
{
    StraightRoad road = MakeStraightRoad(40);
    for (SG::SplineStripStation& station : road.Stations)
    {
        station.HalfWidthLeft = 2.0f;
        station.HalfWidthRight = 2.0f;
    }
    PavingRoadTrim ahead;
    ahead.NodeDistance = 6.0f;
    ahead.StopDistance = 13.0f;
    ahead.Open = true;
    ahead.OpenLeft = Vector3(-2.0f, 0.0f, 13.0f);
    ahead.OpenRight = Vector3(2.0f, 0.0f, 13.0f);
    PavingRoadTrim behind = ahead;
    behind.NodeDistance = 21.0f;
    behind.StopDistance = 15.0f;
    behind.OpenLeft = Vector3(2.0f, 0.0f, 15.0f);
    behind.OpenRight = Vector3(-2.0f, 0.0f, 15.0f);
    const std::vector<PavingRoadTrim> trims = {ahead, behind};

    const TrimmedSweepStations trimmed =
        TrimSweepStations(road.Stations, road.RoadDistance, 0.0f, trims, Mathematics::Matrix4x4::Identity());

    ASSERT_EQ(trimmed.Runs.size(), 3u);
    const auto [first, last] = trimmed.Runs[1];
    ASSERT_EQ(last - first, 2u);
    EXPECT_NEAR(trimmed.Stations[first].RiseScale, 0.0f, kTolerance);
    EXPECT_NEAR(trimmed.Stations[first + 1u].RiseScale, Math::SmoothStep(0.0f, 4.0f, 1.0f), kTolerance);
    EXPECT_NEAR(trimmed.Stations[last].RiseScale, 0.0f, kTolerance);
}

TEST(PavingRoadTrim, AnArmThatEndsTheRoadDropsTheTailWithNoCap)
{
    const StraightRoad road = MakeStraightRoad(40);
    PavingRoadTrim tail;
    tail.NodeDistance = 30.5f;
    tail.StopDistance = 40.0f;

    const TrimmedSweepStations trimmed =
        TrimSweepStations(road.Stations, road.RoadDistance, 0.0f, std::vector<PavingRoadTrim>{tail},
                          Mathematics::Matrix4x4::Identity());

    ASSERT_EQ(trimmed.Runs.size(), 1u);
    const SG::SplineStripStation& last = trimmed.Stations[trimmed.Runs[0].second];
    ExpectNear(last.Position, Vector3(0.0f, 0.0f, 30.5f));
    ExpectNear(last.Right, Vector3(1.0f, 0.0f, 0.0f));
    EXPECT_EQ(trimmed.Source[trimmed.Runs[0].second], 30u);
}

TEST(PavingRoadTrim, AClosedRoadsKeptRunCrossesItsSeamWithRisingDistances)
{
    // A 40 m loop; the region takes 14 to 26, so the strip runs from 26 over the seam to 14.
    const StraightRoad road = MakeStraightRoad(40);
    PavingRoadTrim ahead;
    ahead.NodeDistance = 20.0f;
    ahead.StopDistance = 26.0f;
    PavingRoadTrim behind;
    behind.NodeDistance = 20.0f;
    behind.StopDistance = 14.0f;

    const TrimmedSweepStations trimmed =
        TrimSweepStations(road.Stations, road.RoadDistance, 40.0f, std::vector<PavingRoadTrim>{ahead, behind},
                          Mathematics::Matrix4x4::Identity());

    ASSERT_EQ(trimmed.Runs.size(), 1u);
    const auto [first, last] = trimmed.Runs[0];
    EXPECT_NEAR(trimmed.Stations[first].Distance, 26.0f, kTolerance);
    EXPECT_NEAR(trimmed.Stations[last].Distance, 54.0f, kTolerance);
    EXPECT_NEAR(trimmed.Stations[last].Position.z, 14.0f, kTolerance);
    for (uint32 i = first; i < last; ++i)
        EXPECT_LT(trimmed.Stations[i].Distance, trimmed.Stations[i + 1u].Distance);
}

TEST(PavingRoadTrim, AnArmAcrossAClosedRoadsSeamSkipsBothSidesOfIt)
{
    const StraightRoad road = MakeStraightRoad(40);
    // The node at 2: one arm out to 8, one back over the seam to 36.
    PavingRoadTrim ahead;
    ahead.NodeDistance = 2.0f;
    ahead.StopDistance = 8.0f;
    PavingRoadTrim behind;
    behind.NodeDistance = 2.0f;
    behind.StopDistance = 36.0f;

    const TrimmedSweepStations trimmed =
        TrimSweepStations(road.Stations, road.RoadDistance, 40.0f, std::vector<PavingRoadTrim>{ahead, behind},
                          Mathematics::Matrix4x4::Identity());

    ASSERT_EQ(trimmed.Runs.size(), 1u);
    EXPECT_NEAR(trimmed.Stations[trimmed.Runs[0].first].Position.z, 8.0f, kTolerance);
    EXPECT_NEAR(trimmed.Stations[trimmed.Runs[0].second].Position.z, 36.0f, kTolerance);
}

} // namespace
} // namespace GameEngine::SplineRecipes
