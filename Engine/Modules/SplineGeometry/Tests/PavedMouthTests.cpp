// A road's mouth on a paving (PavedFootprintInput::MouthRadii). Every fixture expects one piece whose outer ring does
// not cross itself or hold a point twice, and whose border turns smoothly (no turn sharper than an arc step, except at
// the outline's corners) and one way through each mouth (no hook).

#include "SplineGeometry/PavedFootprint.h"

#include "Mathematics/Geometry.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::SplineGeometry;
using Mathematics::Vector2;

namespace
{

constexpr float kPi = std::numbers::pi_v<float>;
constexpr float kStationSpacing = 0.5f;
constexpr float kHalfWidth = 1.0f;
constexpr float kRadius = 1.5f;
constexpr float kArmLength = 20.0f;
// An arc point turns at most kRoundCornerStepDegrees; a little over it allows for the points where
// an arc meets a straight edge.
constexpr float kMaxSmoothTurnDegrees = 11.0f;
// A turn under this is straight, so it has no direction.
constexpr float kStraightTurnDegrees = 0.1f;

Vector2 Direction(float degrees)
{
    const float radians = degrees * kPi / 180.0f;
    return Vector2(std::cos(radians), std::sin(radians));
}

// A straight arm from `node` at `degrees`, kArmLength meters, stations every kStationSpacing.
PavedArm StraightArm(uint32 road, const Vector2& node, float degrees)
{
    PavedArm arm;
    arm.Road = road;
    arm.Node = node;
    const int steps = static_cast<int>(std::ceil(kArmLength / kStationSpacing));
    for (int i = 0; i <= steps; ++i)
    {
        const float along = kArmLength * static_cast<float>(i) / static_cast<float>(steps);
        arm.Stations.push_back(PavedArmStation{node + Direction(degrees) * along, kHalfWidth, kHalfWidth, along});
    }
    return arm;
}

// A wide square below z = 0, so a road that leaves its top edge is far from its corners.
AreaRing Square() { return {{-30.0f, -30.0f}, {30.0f, -30.0f}, {30.0f, 0.0f}, {-30.0f, 0.0f}}; }

// A circle of `radius` about (0, -radius), a point every quarter meter, counter-clockwise.
AreaRing Circle(float radius)
{
    AreaRing ring;
    const int count = static_cast<int>(std::ceil(2.0f * kPi * radius / 0.25f));
    for (int i = 0; i < count; ++i)
    {
        const float angle = 2.0f * kPi * static_cast<float>(i) / static_cast<float>(count);
        ring.emplace_back(radius * std::cos(angle), -radius + radius * std::sin(angle));
    }
    return ring;
}

PavedFootprint Build(const AreaRing& outline, const std::vector<PavedArm>& arms, float radius)
{
    const std::vector<AreaRing> outlines{outline};
    const std::vector<float> radii{radius};
    PavedFootprintInput input;
    input.Outlines = outlines;
    input.Arms = arms;
    input.MouthRadii = radii;
    return BuildPavedFootprint(input);
}

bool CrossesItself(const AreaRing& ring)
{
    const std::size_t count = ring.size();
    for (std::size_t i = 0; i < count; ++i)
    {
        for (std::size_t j = i + 2; j < count; ++j)
        {
            if (i == 0 && j == count - 1)
                continue;
            if (Mathematics::SegmentsIntersect(ring[i], ring[(i + 1) % count], ring[j], ring[(j + 1) % count]))
                return true;
        }
    }
    return false;
}

// How a run turns at its inner points, leaving out the points in `corners`.
struct RunTurns
{
    float SharpestDegrees = 0.0f;
    // How often the turn changes from left to right or back.
    int DirectionChanges = 0;
    int MouthEdges = 0;
};

RunTurns TurnsOf(const PavedBoundaryRun& run, const std::vector<Vector2>& corners)
{
    RunTurns turns;
    int lastSign = 0;
    for (std::size_t i = 1; i + 1 < run.Points.size(); ++i)
    {
        const bool atCorner = std::any_of(corners.begin(), corners.end(),
                                        [&run, i](const Vector2& corner) { return (corner - run.Points[i]).Length() < 1.0e-3f; });
        if (atCorner)
            continue;
        const Vector2 in = (run.Points[i] - run.Points[i - 1]).Normalize();
        const Vector2 out = (run.Points[i + 1] - run.Points[i]).Normalize();
        const float degrees =
            std::atan2(in.x * out.y - in.y * out.x, Vector2::Dot(in, out)) * 180.0f / kPi;
        turns.SharpestDegrees = std::max(turns.SharpestDegrees, std::abs(degrees));
        if (std::abs(degrees) < kStraightTurnDegrees)
            continue;
        const int sign = degrees > 0.0f ? 1 : -1;
        if (lastSign != 0 && sign != lastSign)
            ++turns.DirectionChanges;
        lastSign = sign;
    }
    for (const PavedEdge& edge : run.Edges)
        turns.MouthEdges += edge.Source == PavedEdgeSource::Mouth ? 1 : 0;
    return turns;
}

// A footprint of one region and one piece whose outer ring does not cross itself and has no edge
// shorter than kPavedTouchMeters, which would give the lid a triangle with no area.
void ExpectOnePieceThatDoesNotCrossItself(const PavedFootprint& footprint)
{
    ASSERT_EQ(footprint.Regions.size(), 1u);
    ASSERT_EQ(footprint.Regions[0].Pieces.size(), 1u);
    const AreaRing& outer = footprint.Regions[0].Pieces[0].Outer;
    EXPECT_FALSE(CrossesItself(outer));
    for (std::size_t i = 0; i < outer.size(); ++i)
        EXPECT_GT((outer[(i + 1) % outer.size()] - outer[i]).Length(), kPavedTouchMeters) << i;
}

} // namespace

TEST(PavedMouth, AFlaredMouthRoundsBothCornersAtEveryEntryAngle)
{
    const AreaRing square = Square();
    for (const float degrees : {30.0f, 45.0f, 60.0f, 90.0f, 120.0f})
    {
        SCOPED_TRACE(degrees);
        const std::vector<PavedArm> arms{StraightArm(1, Vector2(0.0f, 0.0f), degrees)};
        const PavedFootprint footprint = Build(square, arms, kRadius);
        ExpectOnePieceThatDoesNotCrossItself(footprint);
        ASSERT_EQ(footprint.Regions[0].Runs.size(), 1u);
        const RunTurns turns = TurnsOf(footprint.Regions[0].Runs[0], square);
        EXPECT_LE(turns.SharpestDegrees, kMaxSmoothTurnDegrees);
        // Away from the square's corners, both arcs turn the same way.
        EXPECT_EQ(turns.DirectionChanges, 0);
        EXPECT_GT(turns.MouthEdges, 0);
        // The road goes on past a square cap.
        const PavedArmStop& stop = footprint.Arms[0];
        EXPECT_TRUE(stop.Open);
        EXPECT_NEAR(Vector2::Dot(stop.OpenRight - stop.OpenLeft, Direction(degrees)), 0.0f, 1.0e-3f);
    }
}

TEST(PavedMouth, AMouthAtTheLargestRadiusMeetsTheRoadsSideWithNoBackStep)
{
    // A 6 m road leaves an eight-sided plaza east between two slanted edges. Each 10 m arc turns
    // about 1.4 degrees a point and meets the road's side at a shallow angle. The outline must not
    // step back on itself there: the border would build a spike at the step.
    const AreaRing plaza{{28.0f, 32.0f}, {36.0f, 26.0f}, {48.0f, 27.0f}, {55.0f, 34.0f},
                         {53.0f, 44.0f}, {45.0f, 47.0f}, {38.0f, 52.0f}, {29.0f, 45.0f}};
    PavedArm arm = StraightArm(1, Vector2(54.4f, 37.0f), 0.0f);
    for (PavedArmStation& station : arm.Stations)
        station.HalfWidthLeft = station.HalfWidthRight = 3.0f;

    const PavedFootprint footprint = Build(plaza, {arm}, 10.0f);

    ExpectOnePieceThatDoesNotCrossItself(footprint);
    ASSERT_EQ(footprint.Regions[0].Runs.size(), 1u);
    const RunTurns turns = TurnsOf(footprint.Regions[0].Runs[0], plaza);
    EXPECT_GT(turns.MouthEdges, 0);
    EXPECT_LE(turns.SharpestDegrees, kMaxSmoothTurnDegrees);
}

TEST(PavedMouth, AMouthOnACurvedOutlineMeetsTheCurveWithNoCorner)
{
    const AreaRing circle = Circle(10.0f);
    const std::vector<PavedArm> arms{StraightArm(1, Vector2(0.0f, 0.0f), 60.0f)};

    const PavedFootprint footprint = Build(circle, arms, kRadius);

    ExpectOnePieceThatDoesNotCrossItself(footprint);
    ASSERT_EQ(footprint.Regions[0].Runs.size(), 1u);
    const RunTurns turns = TurnsOf(footprint.Regions[0].Runs[0], {});
    EXPECT_LE(turns.SharpestDegrees, kMaxSmoothTurnDegrees);
    // One way on the first arc, the other way round the circle, back on the second arc.
    EXPECT_EQ(turns.DirectionChanges, 2);
    EXPECT_GT(turns.MouthEdges, 0);
}

TEST(PavedMouth, ARoadThroughACurvedOutlineGetsAMouthAndNoHoleUnderItsNode)
{
    // At the circle's top both sides of a square entry start outside the circle.
    const AreaRing circle = Circle(10.0f);
    for (const float degrees : {90.0f, 80.0f})
    {
        SCOPED_TRACE(degrees);
        PavedArm inward = StraightArm(1, Vector2(0.0f, 0.0f), degrees + 180.0f);
        for (PavedArmStation& station : inward.Stations)
            station.Distance = -station.Distance;
        // Half the road's length inside the circle, so the arm ends inside it.
        inward.Stations.resize(inward.Stations.size() / 2);
        const std::vector<PavedArm> arms{StraightArm(1, Vector2(0.0f, 0.0f), degrees), inward};

        const PavedFootprint footprint = Build(circle, arms, kRadius);

        ExpectOnePieceThatDoesNotCrossItself(footprint);
        EXPECT_TRUE(footprint.Regions[0].Pieces[0].Holes.empty());
        ASSERT_EQ(footprint.Regions[0].Runs.size(), 1u);
        EXPECT_GT(TurnsOf(footprint.Regions[0].Runs[0], {}).MouthEdges, 0);
    }
}

TEST(PavedMouth, TwoMouthsCloserThanTwoRadiiGetArcsThatMeetAndDoNotCross)
{
    // 1.5 m of the square's edge between the two roads: under two radii.
    const AreaRing square = Square();
    const std::vector<PavedArm> arms{StraightArm(1, Vector2(0.0f, 0.0f), 90.0f),
                                     StraightArm(2, Vector2(3.5f, 0.0f), 90.0f)};

    const PavedFootprint footprint = Build(square, arms, kRadius);

    ExpectOnePieceThatDoesNotCrossItself(footprint);
    const AreaRing& outer = footprint.Regions[0].Pieces[0].Outer;
    // The two arcs meet halfway between the roads, on the square's edge.
    const auto halfway = std::find_if(outer.begin(), outer.end(),
                                      [](const Vector2& point) { return (point - Vector2(1.75f, 0.0f)).Length() < 0.01f; });
    EXPECT_NE(halfway, outer.end());
    for (const PavedBoundaryRun& run : footprint.Regions[0].Runs)
        EXPECT_LE(TurnsOf(run, square).SharpestDegrees, kMaxSmoothTurnDegrees);
}

TEST(PavedMouth, ARoadThatLeavesAlongTheOutlineJoinsStraight)
{
    // At 3 degrees the arc that fits before the arm's end is under kMinPavedMouthRadiusMeters.
    const AreaRing square = Square();
    for (const float degrees : {0.0f, 3.0f})
    {
        SCOPED_TRACE(degrees);
        const std::vector<PavedArm> arms{StraightArm(1, Vector2(0.0f, 0.0f), degrees)};

        const PavedFootprint flared = Build(square, arms, kRadius);
        const PavedFootprint straight = Build(square, arms, 0.0f);

        ExpectOnePieceThatDoesNotCrossItself(flared);
        ASSERT_EQ(straight.Regions.size(), 1u);
        EXPECT_EQ(flared.Regions[0].Pieces[0].Outer, straight.Regions[0].Pieces[0].Outer);
        EXPECT_FLOAT_EQ(flared.Arms[0].StopDistance, straight.Arms[0].StopDistance);
    }
}

TEST(PavedMouth, AMouthsStopMovesPastATurnJustBeyondTheThroat)
{
    // The road leaves the square north, turns 110 degrees right 2.5 m out, within one width of the
    // 1.5 m throat, and turns north again 3 m on: a cap at the throat would cut across the turned
    // leg's stations.
    PavedArm arm = StraightArm(1, Vector2(0.0f, 0.0f), 90.0f);
    arm.Stations.resize(6);
    for (const auto& [degrees, steps] : {std::pair{-20.0f, 6}, std::pair{90.0f, 30}})
    {
        for (int i = 0; i < steps; ++i)
        {
            const PavedArmStation& last = arm.Stations.back();
            arm.Stations.push_back(PavedArmStation{last.Position + Direction(degrees) * kStationSpacing, kHalfWidth,
                                                   kHalfWidth, last.Distance + kStationSpacing});
        }
    }

    const PavedFootprint footprint = Build(Square(), {arm}, kRadius);

    ExpectOnePieceThatDoesNotCrossItself(footprint);
    EXPECT_TRUE(footprint.Arms[0].Open);
    EXPECT_GT(footprint.Arms[0].StopDistance, 2.5f) << "the stop stays short of the turn";
}

TEST(PavedMouth, AnArmWhoseNodeIsInsideTheOutlineGetsNoMouth)
{
    // The node lies 5 m inside the square; the road leaves it 5 m out from the node.
    const std::vector<PavedArm> arms{StraightArm(1, Vector2(0.0f, -5.0f), 90.0f)};

    const PavedFootprint flared = Build(Square(), arms, kRadius);
    const PavedFootprint straight = Build(Square(), arms, 0.0f);

    ASSERT_EQ(flared.Regions.size(), 1u);
    ASSERT_EQ(straight.Regions.size(), 1u);
    EXPECT_EQ(flared.Regions[0].Pieces[0].Outer, straight.Regions[0].Pieces[0].Outer);
}

TEST(PavedMouth, AnArcReachesAlongTheRoadAtMostThreeWidthsPastTheCorner)
{
    // At 10 degrees a 1.5 m arc on the sharp side would end 20 m along the road. The right side
    // leaves the square 1 / sin(10 degrees) m along the road; the arc ends at most three widths on.
    const AreaRing square = Square();
    const float corner = kHalfWidth / std::sin(10.0f * kPi / 180.0f);
    const std::vector<PavedArm> arms{StraightArm(1, Vector2(0.0f, 0.0f), 10.0f)};

    const PavedFootprint footprint = Build(square, arms, kRadius);

    ExpectOnePieceThatDoesNotCrossItself(footprint);
    ASSERT_EQ(footprint.Regions[0].Runs.size(), 1u);
    EXPECT_GT(TurnsOf(footprint.Regions[0].Runs[0], square).MouthEdges, 0);
    EXPECT_LE(footprint.Arms[0].StopDistance, corner + 3.0f * 2.0f * kHalfWidth + 0.01f);
}

TEST(PavedMouth, AThinCurvedPlazaGetsAMouthAndNoHole)
{
    // An ellipse 20 m by 1.3 m below z = 0, a road leaving it at 60 degrees from a node 1 m from its
    // end and 0.3 m below its top edge, nearer that edge than the far one.
    AreaRing ellipse;
    for (int i = 0; i < 240; ++i)
    {
        const float angle = 2.0f * kPi * static_cast<float>(i) / 240.0f;
        ellipse.emplace_back(10.0f * std::cos(angle), -0.65f + 0.65f * std::sin(angle));
    }
    const std::vector<PavedArm> arms{StraightArm(1, Vector2(9.0f, -0.3f), 60.0f)};
    for (const float radius : {0.0f, kRadius})
    {
        SCOPED_TRACE(radius);
        const PavedFootprint footprint = Build(ellipse, arms, radius);
        ExpectOnePieceThatDoesNotCrossItself(footprint);
        EXPECT_TRUE(footprint.Regions[0].Pieces[0].Holes.empty());
    }
}
