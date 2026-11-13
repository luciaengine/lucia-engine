// A paving region's border (BuildPavedBorder): its boundary runs as strip stations, ready for
// BuildSplineStrip with the border's Rectangle profile.

#include "SplineGeometry/AreaGround.h"
#include "SplineGeometry/PavedBorder.h"
#include "SplineGeometry/PavedFootprint.h"

#include "Mathematics/Geometry.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::SplineGeometry;
using Mathematics::Vector2;
using Mathematics::Vector3;

namespace
{

constexpr float kStationSpacing = 0.5f;

AreaRing Rectangle(float minX, float minZ, float maxX, float maxZ)
{
    return {{minX, minZ}, {maxX, minZ}, {maxX, maxZ}, {minX, maxZ}};
}

PavedArm StraightArm(uint32 road, const Vector2& node, const Vector2& direction, float length, float halfWidth)
{
    PavedArm arm;
    arm.Road = road;
    arm.Node = node;
    const int steps = static_cast<int>(std::ceil(length / kStationSpacing));
    for (int i = 0; i <= steps; ++i)
    {
        const float along = length * static_cast<float>(i) / static_cast<float>(steps);
        arm.Stations.push_back(PavedArmStation{node + direction * along, halfWidth, halfWidth, along});
    }
    return arm;
}

float Flat(const Vector2&) { return 0.0f; }

// The runs of the one region `outlines`, `arms` and `excludes` make.
std::vector<PavedBoundaryRun> RunsOf(const std::vector<AreaRing>& outlines, const std::vector<PavedArm>& arms,
                                     const std::vector<AreaRing>& excludes)
{
    const PavedFootprint footprint = BuildPavedFootprint(PavedFootprintInput{outlines, arms, excludes});
    EXPECT_EQ(footprint.Regions.size(), 1u);
    return footprint.Regions.empty() ? std::vector<PavedBoundaryRun>{} : footprint.Regions[0].Runs;
}

// An L-shaped plaza no road enters, with a planter cut out of it.
std::vector<PavedBoundaryRun> PlazaWithPlanter()
{
    return RunsOf({{{0.0f, 0.0f}, {30.0f, 0.0f}, {30.0f, 12.0f}, {12.0f, 12.0f}, {12.0f, 30.0f}, {0.0f, 30.0f}}}, {},
                  {Rectangle(3.0f, 3.0f, 8.0f, 8.0f)});
}

// The cosine between a station's forward and the edge from `from` to `to`, on the ground plane.
float ForwardAlong(const SplineStripStation& station, const Vector2& from, const Vector2& to)
{
    const Vector2 edge = (to - from).Normalize();
    return station.Forward.x * edge.x + station.Forward.z * edge.y;
}

// Distance never falls along a run, and rises at every station that advances.
void ExpectDistanceContinuous(const PavedBorderRun& border)
{
    for (std::size_t i = 1; i < border.Stations.size(); ++i)
    {
        const SplineStripStation& station = border.Stations[i];
        if (station.Join == SplineStationJoin::Advance)
            EXPECT_GT(station.Distance, border.Stations[i - 1].Distance) << "station " << i;
        else
            EXPECT_EQ(station.Distance, border.Stations[i - 1].Distance) << "station " << i;
    }
}

// The station whose distance along the run is nearest `distance`.
const SplineStripStation& NearestStation(const std::vector<SplineStripStation>& stations, float distance)
{
    std::size_t nearest = 0;
    for (std::size_t i = 1; i < stations.size(); ++i)
    {
        if (std::abs(stations[i].Distance - distance) < std::abs(stations[nearest].Distance - distance))
            nearest = i;
    }
    return stations[nearest];
}

} // namespace

// A plaza no road enters has a closed border on its rim and one round its planter: each a loop
// whose last station is its first, its U running on from station to station and every face
// meeting itself at the seam on a whole tile, its right of travel out of the paving (into the
// planter on the hole's), and its top the border's height above the lid.
TEST(PavedBorder, AClosedRunIsOneLoopOnWholeTilesWithTheHolesBorderInsideTheHole)
{
    const PavedBorderSettings settings;
    const std::vector<PavedBoundaryRun> runs = PlazaWithPlanter();
    ASSERT_EQ(runs.size(), 2u);
    const std::vector<PavedBorderRun> borders = BuildPavedBorder(runs, settings, Flat, {});
    ASSERT_EQ(borders.size(), 2u);
    const SplineProfile profile = BuildPavedBorderProfile(settings);
    for (std::size_t r = 0; r < borders.size(); ++r)
    {
        const PavedBorderRun& border = borders[r];
        ASSERT_TRUE(border.Closed);
        ASSERT_GE(border.Stations.size(), 4u);
        EXPECT_EQ(border.Stations.front().Position.x, border.Stations.back().Position.x);
        EXPECT_EQ(border.Stations.front().Position.z, border.Stations.back().Position.z);
        ExpectDistanceContinuous(border);
        EXPECT_NEAR(border.Strip.LoopLengthMetres, border.Stations.back().Distance - border.Stations.front().Distance,
                    1.0e-4f);
        EXPECT_FALSE(border.Strip.CapStart);
        EXPECT_FALSE(border.Strip.CapEnd);

        for (const SplineStripStation& station : border.Stations)
        {
            // At a corner the right of one leg runs along the other.
            const bool atCorner = std::any_of(runs[r].Points.begin(), runs[r].Points.end(), [&](const Vector2& point) {
                return std::hypot(point.x - station.Position.x, point.y - station.Position.z) < 1.0e-3f;
            });
            if (atCorner)
                continue;
            const Vector2 outward(station.Position.x + station.Right.x * 0.05f, station.Position.z + station.Right.z * 0.05f);
            const bool inPlanter = Mathematics::PointInPolygon(outward, Rectangle(3.0f, 3.0f, 8.0f, 8.0f));
            EXPECT_EQ(inPlanter, runs[r].Hole) << "run " << r << ": the border's right is not out of the paving";
        }

        const SplineStripMesh mesh = BuildSplineStrip(profile, border.Stations, border.Strip);
        ASSERT_TRUE(mesh.IsValid());
        EXPECT_EQ(mesh.DroppedStations, 0u);
        const uint32 last = (mesh.RingCount - 1) * mesh.RingVertexCount;
        float top = -1.0f;
        for (uint32 k = 0; k < mesh.RingVertexCount; ++k)
        {
            const float tiles = mesh.Vertices[last + k].UV.x - mesh.Vertices[k].UV.x;
            EXPECT_NEAR(tiles, std::round(tiles), 1.0e-3f) << "run " << r << ", ring vertex " << k << " at the seam";
            top = std::max(top, mesh.Vertices[k].Position.y);
        }
        EXPECT_NEAR(top, settings.Height, 1.0e-4f) << "the border's top above the lid";
    }
}

// Where a road leaves the region the runs stop: each open run is capped at both ends, its first
// and last stations facing along the run's first and last edges, so the border ends square to the
// Open edge beside it.
TEST(PavedBorder, AnOpenRunIsCappedAndEndsSquareToTheOpenEdge)
{
    const std::vector<PavedBoundaryRun> runs =
        RunsOf({Rectangle(-10.0f, -10.0f, 10.0f, 10.0f)},
               {StraightArm(0, {0.0f, 10.0f}, {0.0f, 1.0f}, 20.0f, 2.0f),
                StraightArm(1, {10.0f, 0.0f}, {1.0f, 0.0f}, 20.0f, 2.0f)},
               {});
    ASSERT_EQ(runs.size(), 2u);
    const std::vector<PavedBorderRun> borders = BuildPavedBorder(runs, PavedBorderSettings{}, Flat, {});
    for (std::size_t r = 0; r < borders.size(); ++r)
    {
        const PavedBorderRun& border = borders[r];
        const PavedBoundaryRun& run = runs[r];
        ASSERT_FALSE(border.Closed);
        EXPECT_TRUE(border.Strip.CapStart);
        EXPECT_TRUE(border.Strip.CapEnd);
        ExpectDistanceContinuous(border);
        EXPECT_NEAR(ForwardAlong(border.Stations.front(), run.Points[0], run.Points[1]), 1.0f, 1.0e-4f);
        EXPECT_NEAR(ForwardAlong(border.Stations.back(), run.Points[run.Points.size() - 2], run.Points.back()), 1.0f,
                    1.0e-4f);
        EXPECT_TRUE(BuildSplineStrip(BuildPavedBorderProfile(PavedBorderSettings{}), border.Stations, border.Strip).IsValid());
    }
}

// A run end on the Open edge of an arm whose road has a verge has no cap, and the border there takes
// the verge's width and height, so its last ring is the verge's first. The end on the other arm's
// Open edge keeps its cap and the border's own size.
TEST(PavedBorder, ARunEndAtAVergedRoadHasNoCapAndTakesTheVergesSize)
{
    const std::vector<PavedBoundaryRun> runs =
        RunsOf({Rectangle(-10.0f, -10.0f, 10.0f, 10.0f)},
               {StraightArm(0, {0.0f, 10.0f}, {0.0f, 1.0f}, 20.0f, 2.0f),
                StraightArm(1, {10.0f, 0.0f}, {1.0f, 0.0f}, 20.0f, 2.0f)},
               {});
    ASSERT_EQ(runs.size(), 2u);
    const PavedBorderSettings settings;
    const std::vector<PavedVerge> verges = {PavedVerge{true, 0.45f, 0.2f}, PavedVerge{}};
    const std::vector<PavedBorderRun> capped = BuildPavedBorder(runs, settings, Flat, {});
    const std::vector<PavedBorderRun> verged = BuildPavedBorder(runs, settings, Flat, verges);
    const SplineProfile profile = BuildPavedBorderProfile(settings);
    for (std::size_t r = 0; r < runs.size(); ++r)
    {
        const PavedBoundaryRun& run = runs[r];
        ASSERT_EQ(run.StartBreak.Source, PavedEdgeSource::Open);
        ASSERT_EQ(run.EndBreak.Source, PavedEdgeSource::Open);
        ASSERT_NE(run.StartBreak.Index, run.EndBreak.Index);
        const PavedBorderRun& border = verged[r];
        const bool startVerged = run.StartBreak.Index == 0u;
        EXPECT_EQ(border.Strip.CapStart, !startVerged);
        EXPECT_EQ(border.Strip.CapEnd, startVerged);
        const SplineStripStation& atVerge = startVerged ? border.Stations.front() : border.Stations.back();
        const SplineStripStation& atRoad = startVerged ? border.Stations.back() : border.Stations.front();
        EXPECT_NEAR(atVerge.HalfWidthRight, 0.45f, 1.0e-5f);
        EXPECT_NEAR(atVerge.TopOffset, 0.2f - settings.Height, 1.0e-5f);
        EXPECT_NEAR(atRoad.HalfWidthRight, settings.Width, 1.0e-5f);
        EXPECT_EQ(atRoad.TopOffset, 0.0f);
        const SplineStripStation& middle = border.Stations[border.Stations.size() / 2u];
        EXPECT_EQ(middle.HalfWidthRight, settings.Width);
        EXPECT_EQ(middle.TopOffset, 0.0f);

        // One cap fewer: the Rectangle's four points.
        const SplineStripMesh withCaps = BuildSplineStrip(profile, capped[r].Stations, capped[r].Strip);
        const SplineStripMesh withVerge = BuildSplineStrip(profile, border.Stations, border.Strip);
        EXPECT_EQ(withCaps.Vertices.size() - withVerge.Vertices.size(), profile.Points.size());
    }
}

// A run of Mouth edges between two verged roads: each end takes its own road's verge over its own
// arm's edges, so the top stands at the verge's height at both seams with no step between them.
TEST(PavedBorder, ARunBetweenTwoVergedRoadsTakesEachVergeAtItsOwnEnd)
{
    PavedBoundaryRun run;
    run.Points = {Vector2(0.0f, 0.0f), Vector2(1.0f, 0.3f), Vector2(2.0f, 0.0f)};
    run.Edges = {PavedEdge{PavedEdgeSource::Mouth, 0u}, PavedEdge{PavedEdgeSource::Mouth, 1u}};
    run.NewPoints = {0, 0, 0};
    run.StartBreak = PavedEdge{PavedEdgeSource::Open, 0u};
    run.EndBreak = PavedEdge{PavedEdgeSource::Open, 1u};
    run.StartBreakDirection = Vector2(0.0f, 1.0f);
    run.EndBreakDirection = Vector2(0.0f, -1.0f);
    PavedBorderSettings settings;
    settings.StationSpacingMeters = 0.25f;
    const std::vector<PavedVerge> verges = {PavedVerge{true, 0.3f, 0.2f}, PavedVerge{true, 0.3f, 0.2f}};
    const std::vector<PavedBoundaryRun> runs = {run};
    const std::vector<PavedBorderRun> borders = BuildPavedBorder(runs, settings, Flat, verges);
    ASSERT_EQ(borders.size(), 1u);
    const std::vector<SplineStripStation>& stations = borders[0].Stations;
    EXPECT_NEAR(stations.front().TopOffset, 0.2f - settings.Height, 1.0e-5f);
    EXPECT_NEAR(stations.back().TopOffset, 0.2f - settings.Height, 1.0e-5f);
    for (std::size_t i = 1; i < stations.size(); ++i)
        EXPECT_LT(std::abs(stations[i].TopOffset - stations[i - 1].TopOffset), 0.03f) << "station " << i;
}

// At a rounded mouth the border takes the verge's size along the mouth's arc: halfway along the arc
// the width and the height lie between the curb's and the verge's, and past the arc the curb keeps its own.
TEST(PavedBorder, TheBorderTakesTheVergesSizeAlongTheMouthsArc)
{
    const std::vector<AreaRing> outlines = {Rectangle(-10.0f, -10.0f, 10.0f, 10.0f)};
    const std::vector<PavedArm> arms = {StraightArm(0, {0.0f, 10.0f}, {0.0f, 1.0f}, 20.0f, 2.0f)};
    const std::vector<float32> radii = {2.0f};
    PavedFootprintInput input{outlines, arms, {}};
    input.MouthRadii = radii;
    const PavedFootprint footprint = BuildPavedFootprint(input);
    ASSERT_EQ(footprint.Regions.size(), 1u);
    ASSERT_EQ(footprint.Regions[0].Runs.size(), 1u);
    const PavedBoundaryRun& run = footprint.Regions[0].Runs[0];
    float arc = 0.0f;
    for (std::size_t edge = 0; edge < run.Edges.size() && run.Edges[edge].Source == PavedEdgeSource::Mouth; ++edge)
        arc += (run.Points[edge + 1u] - run.Points[edge]).Length();
    ASSERT_GT(arc, 1.0f);

    PavedBorderSettings settings;
    const std::vector<PavedVerge> verges = {PavedVerge{true, 0.6f, 0.2f}};
    const std::vector<PavedBorderRun> borders = BuildPavedBorder(footprint.Regions[0].Runs, settings, Flat, verges);
    ASSERT_EQ(borders.size(), 1u);
    const std::vector<SplineStripStation>& stations = borders[0].Stations;
    const SplineStripStation& halfway = NearestStation(stations, stations.front().Distance + arc * 0.5f);
    EXPECT_GT(halfway.HalfWidthRight, settings.Width + 0.05f);
    EXPECT_LT(halfway.HalfWidthRight, 0.6f - 0.05f);
    EXPECT_GT(halfway.TopOffset, 0.01f);
    EXPECT_LT(halfway.TopOffset, 0.2f - settings.Height - 0.01f);
    const SplineStripStation& pastArc = NearestStation(stations, stations.front().Distance + arc + 0.5f);
    EXPECT_NEAR(pastArc.HalfWidthRight, settings.Width, 1.0e-5f);
    EXPECT_NEAR(pastArc.TopOffset, 0.0f, 1.0e-5f);
}

// The distance from one end of `run` to the far end of the Mouth edges of `arm` there, past the
// side of the arm that lies between that end and the arc.
float MouthArcAtEnd(const PavedBoundaryRun& run, bool atEnd, uint32 arm)
{
    float walked = 0.0f;
    float arc = 0.0f;
    for (std::size_t step = 0; step < run.Edges.size(); ++step)
    {
        const std::size_t edge = atEnd ? run.Edges.size() - 1u - step : step;
        const PavedEdge& source = run.Edges[edge];
        if (source.Index != arm || (source.Source != PavedEdgeSource::Mouth && source.Source != PavedEdgeSource::ArmSide))
            break;
        walked += (run.Points[edge + 1u] - run.Points[edge]).Length();
        if (source.Source == PavedEdgeSource::Mouth)
            arc = walked;
    }
    return arc;
}

// Halfway along the mouth's arc at the end of `stations` the width and the height lie between the
// curb's and the verge's.
void ExpectHalfwayBetween(const std::vector<SplineStripStation>& stations, bool atEnd, float arc,
                          const PavedBorderSettings& settings, const PavedVerge& verge)
{
    const float end = atEnd ? stations.back().Distance : stations.front().Distance;
    const SplineStripStation& halfway = NearestStation(stations, end + (atEnd ? -arc : arc) * 0.5f);
    EXPECT_GT(halfway.HalfWidthRight, settings.Width + 0.05f) << (atEnd ? "end" : "start");
    EXPECT_LT(halfway.HalfWidthRight, verge.Width - 0.05f) << (atEnd ? "end" : "start");
    EXPECT_GT(halfway.TopOffset, 0.01f) << (atEnd ? "end" : "start");
    EXPECT_LT(halfway.TopOffset, verge.Height - settings.Height - 0.01f) << (atEnd ? "end" : "start");
}

// Both sides of a rounded mouth take the verge's size along their own arc, whatever the verged
// arm's index. The road meets the edge at a slant, so one side of the arm lies between the Open
// edge and that side's arc.
TEST(PavedBorder, BothSidesOfAMouthTakeTheVergesSizeAlongTheirArc)
{
    const std::vector<AreaRing> outlines = {Rectangle(-10.0f, -10.0f, 10.0f, 10.0f)};
    const PavedVerge verge{true, 0.6f, 0.2f};
    const PavedBorderSettings settings;
    for (const uint32 vergedArm : {0u, 1u})
    {
        SCOPED_TRACE(vergedArm);
        std::vector<PavedArm> arms = {StraightArm(vergedArm, {0.0f, 10.0f}, Vector2(0.2f, 1.0f).Normalize(), 20.0f, 2.0f)};
        std::vector<PavedVerge> verges(vergedArm + 1u);
        verges[vergedArm] = verge;
        if (vergedArm == 1u)
            arms.insert(arms.begin(), StraightArm(0, {-10.0f, 0.0f}, {-1.0f, 0.0f}, 20.0f, 2.0f));
        const std::vector<float32> radii(arms.size(), 2.0f);
        PavedFootprintInput input{outlines, arms, {}};
        input.MouthRadii = radii;
        const PavedFootprint footprint = BuildPavedFootprint(input);
        ASSERT_EQ(footprint.Regions.size(), 1u);
        const std::vector<PavedBoundaryRun>& runs = footprint.Regions[0].Runs;
        const std::vector<PavedBorderRun> borders = BuildPavedBorder(runs, settings, Flat, verges);
        ASSERT_EQ(borders.size(), runs.size());
        int sides = 0;
        for (std::size_t r = 0; r < runs.size(); ++r)
        {
            for (const bool atEnd : {false, true})
            {
                const PavedEdge& open = atEnd ? runs[r].EndBreak : runs[r].StartBreak;
                if (open.Source != PavedEdgeSource::Open || open.Index != vergedArm)
                    continue;
                const float arc = MouthArcAtEnd(runs[r], atEnd, vergedArm);
                ASSERT_GT(arc, 1.0f) << (atEnd ? "end" : "start");
                ExpectHalfwayBetween(borders[r].Stations, atEnd, arc, settings, verge);
                ++sides;
            }
        }
        EXPECT_EQ(sides, 2);
    }
}

// A short arc of one road beside a long arc of another: the verge's size change stays on its own
// road's arc and does not run on along the other road's arc.
TEST(PavedBorder, TheVergesSizeChangeStopsAtTheEndOfItsOwnRoadsArc)
{
    PavedBoundaryRun run;
    run.Points = {Vector2(0.0f, 0.0f), Vector2(0.4f, 0.0f), Vector2(3.0f, 0.0f)};
    run.Edges = {PavedEdge{PavedEdgeSource::Mouth, 0u}, PavedEdge{PavedEdgeSource::Mouth, 1u}};
    run.NewPoints = {0, 0, 0};
    run.StartBreak = PavedEdge{PavedEdgeSource::Open, 0u};
    run.EndBreak = PavedEdge{PavedEdgeSource::Open, 1u};
    run.StartBreakDirection = Vector2(0.0f, 1.0f);
    run.EndBreakDirection = Vector2(0.0f, -1.0f);
    PavedBorderSettings settings;
    settings.StationSpacingMeters = 0.25f;
    const std::vector<PavedVerge> verges = {PavedVerge{true, 0.5f, 0.2f}, PavedVerge{false, 0.3f, 0.12f}};
    const std::vector<PavedBoundaryRun> runs = {run};
    const std::vector<PavedBorderRun> borders = BuildPavedBorder(runs, settings, Flat, verges);
    ASSERT_EQ(borders.size(), 1u);
    const std::vector<SplineStripStation>& stations = borders[0].Stations;
    const SplineStripStation& pastArc = NearestStation(stations, stations.front().Distance + 1.0f);
    EXPECT_NEAR(pastArc.HalfWidthRight, settings.Width, 1.0e-5f);
    EXPECT_NEAR(pastArc.TopOffset, 0.0f, 1.0e-5f);
}

// The points the boolean made take the border's corner style: a T junction's two notches are
// mitered (one crease ring each) or rounded (a fan); two roads crossing at 30 degrees turn their
// acute notches past the miter limit, which builds them round and says so.
TEST(PavedBorder, NewPointsTakeTheCornerStyleWithTheMiterLimit)
{
    std::vector<PavedArm> arms{StraightArm(0, {0.0f, 0.0f}, {1.0f, 0.0f}, 30.0f, 2.75f),
                               StraightArm(0, {0.0f, 0.0f}, {-1.0f, 0.0f}, 30.0f, 2.75f),
                               StraightArm(1, {0.0f, 0.0f}, {0.0f, 1.0f}, 30.0f, 1.8f)};
    // The westward arm's arc length falls from the node.
    for (PavedArmStation& station : arms[1].Stations)
        station.Distance = -station.Distance;
    const std::vector<PavedBoundaryRun> runs = RunsOf({}, arms, {});
    int newPoints = 0;
    for (const PavedBoundaryRun& run : runs)
        newPoints += static_cast<int>(std::count(run.NewPoints.begin(), run.NewPoints.end(), uint8{1}));
    ASSERT_EQ(newPoints, 2);

    PavedBorderSettings settings;
    int creases = 0;
    for (const PavedBorderRun& border : BuildPavedBorder(runs, settings, Flat, {}))
    {
        EXPECT_TRUE(border.CornerIssues.empty());
        creases += static_cast<int>(std::count_if(border.Stations.begin(), border.Stations.end(),
                                                  [](const SplineStripStation& s) { return s.Join == SplineStationJoin::Crease; }));
    }
    EXPECT_EQ(creases, 2) << "a miter is one ring emitted twice";

    settings.Corner = SplineCornerStyle::Round;
    int pivots = 0;
    for (const PavedBorderRun& border : BuildPavedBorder(runs, settings, Flat, {}))
        pivots += static_cast<int>(std::count_if(border.Stations.begin(), border.Stations.end(),
                                                 [](const SplineStripStation& s) { return s.Join == SplineStationJoin::Pivot; }));
    EXPECT_GE(pivots, 2 * 8) << "a 90 degree round corner is a fan of 9 rings";

    const float cos30 = std::sqrt(3.0f) * 0.5f;
    std::vector<PavedArm> acute{StraightArm(0, {0.0f, 0.0f}, {1.0f, 0.0f}, 40.0f, 2.0f),
                                StraightArm(0, {0.0f, 0.0f}, {-1.0f, 0.0f}, 40.0f, 2.0f),
                                StraightArm(1, {0.0f, 0.0f}, {cos30, 0.5f}, 40.0f, 2.0f),
                                StraightArm(1, {0.0f, 0.0f}, {-cos30, -0.5f}, 40.0f, 2.0f)};
    for (const std::size_t back : {std::size_t{1}, std::size_t{3}})
        for (PavedArmStation& station : acute[back].Stations)
            station.Distance = -station.Distance;
    int pastLimit = 0;
    for (const PavedBorderRun& border : BuildPavedBorder(RunsOf({}, acute, {}), PavedBorderSettings{}, Flat, {}))
        pastLimit += static_cast<int>(std::count_if(border.CornerIssues.begin(), border.CornerIssues.end(),
                                                    [](const SplineCornerIssue& issue) {
                                                        return issue.Kind == SplineCornerIssueKind::PastMiterLimit;
                                                    }));
    EXPECT_EQ(pastLimit, 2) << "the two acute notches turn 150 degrees";
}

// On ground that falls away from the paving the border's base sinks by the fall across its width,
// so it never floats; where the ground rises away from the paving it does not sink.
TEST(PavedBorder, TheBaseSinksWhereTheGroundFallsAwayUnderIt)
{
    const PavedBorderSettings settings;
    const auto slope = [](const Vector2& xz) { return 0.2f * xz.x; };
    const std::vector<PavedBoundaryRun> runs = RunsOf({Rectangle(0.0f, 0.0f, 10.0f, 10.0f)}, {}, {});
    ASSERT_EQ(runs.size(), 1u);
    const std::vector<PavedBorderRun> borders = BuildPavedBorder(runs, settings, slope, {});
    int west = 0;
    int east = 0;
    for (const SplineStripStation& station : borders[0].Stations)
    {
        EXPECT_NEAR(station.Position.y, slope(Vector2(station.Position.x, station.Position.z)), 1.0e-5f);
        if (station.Position.x < 1.0e-4f && station.Position.z > 1.0f && station.Position.z < 9.0f)
        {
            EXPECT_NEAR(station.GroundOffset, -0.2f * settings.Width - kPavedBorderFootingMeters, 1.0e-4f)
                << "the west side falls outward";
            ++west;
        }
        if (station.Position.x > 10.0f - 1.0e-4f && station.Position.z > 1.0f && station.Position.z < 9.0f)
        {
            EXPECT_NEAR(station.GroundOffset, -kPavedBorderFootingMeters, 1.0e-4f) << "the east side rises outward";
            ++east;
        }
    }
    EXPECT_GT(west, 10);
    EXPECT_GT(east, 10);
}

// The paving lifts its stations above the ground; the border's base still stands in the ground at
// every station, on both sides of the border.
TEST(PavedBorder, TheBaseReachesBelowTheGroundUnderALiftedRun)
{
    constexpr float kLift = 0.05f;
    const PavedBorderSettings settings;
    const auto ground = [](const Vector2& xz) { return 0.2f * xz.x + 0.1f * xz.y; };
    const auto lifted = [&ground](const Vector2& xz) { return ground(xz) + kLift; };
    const std::vector<PavedBoundaryRun> runs = RunsOf({Rectangle(0.0f, 0.0f, 10.0f, 10.0f)}, {}, {});
    ASSERT_EQ(runs.size(), 1u);
    const std::vector<PavedBorderRun> borders = BuildPavedBorder(runs, settings, lifted, {});
    ASSERT_FALSE(borders[0].Stations.empty());
    for (const SplineStripStation& station : borders[0].Stations)
    {
        for (const float side : {-station.HalfWidthLeft, station.HalfWidthRight})
        {
            const Vector3 base = station.Position + station.Right * side + station.Up * station.GroundOffset;
            EXPECT_LT(base.y, ground(Vector2(base.x, base.z)) - 0.1f)
                << "station at " << station.Position.x << ", " << station.Position.z;
        }
    }
}

// On a round plaza the lid's corners skip ring samples, and its split stands on the samples it
// skips, so the lid's outline lies on the ring; the border reaches kAreaLidInsetMeters in over the
// lid, so no gap opens at its foot.
TEST(PavedBorder, TheBorderReachesOverTheLidsEdge)
{
    // A 30 m circle with points 0.5 m apart: the lid's corners skip about three of them at a time.
    AreaRing circle;
    constexpr int kPoints = 377;
    for (int i = 0; i < kPoints; ++i)
    {
        const float angle = 2.0f * std::numbers::pi_v<float> * static_cast<float>(i) / static_cast<float>(kPoints);
        circle.emplace_back(30.0f * std::cos(angle), 30.0f * std::sin(angle));
    }
    const PavedFootprint footprint = BuildPavedFootprint(PavedFootprintInput{std::vector<AreaRing>{circle}, {}, {}});
    ASSERT_EQ(footprint.Regions.size(), 1u);
    const PavedRegion& region = footprint.Regions[0];
    const AreaGround lid = BuildAreaGround(region.Pieces, AreaGroundSettings{}, Flat);
    // The lid's boundary edges are the edges of one triangle each; how far inside the ring their
    // ends and middles lie.
    std::vector<std::pair<uint32, uint32>> edges;
    for (std::size_t t = 0; t + 2 < lid.LidTriangles.size(); t += 3)
        for (std::size_t k = 0; k < 3; ++k)
            edges.emplace_back(std::minmax(lid.LidTriangles[t + k], lid.LidTriangles[t + (k + 1) % 3]));
    std::sort(edges.begin(), edges.end());
    float deepest = 0.0f;
    for (std::size_t i = 0; i < edges.size(); ++i)
    {
        const bool shared = (i > 0 && edges[i - 1] == edges[i]) || (i + 1 < edges.size() && edges[i + 1] == edges[i]);
        if (shared)
            continue;
        const Vector3& a = lid.LidPoints[edges[i].first];
        const Vector3& b = lid.LidPoints[edges[i].second];
        for (const Vector2& point : {Vector2(a.x, a.z), Vector2((a.x + b.x) * 0.5f, (a.z + b.z) * 0.5f)})
        {
            float nearest = std::numeric_limits<float>::max();
            const std::vector<Vector2>& ring = region.Runs[0].Points;
            for (std::size_t j = 0; j < ring.size(); ++j)
                nearest = std::min(nearest, Mathematics::DistanceToSegment(point, ring[j], ring[(j + 1) % ring.size()]));
            deepest = std::max(deepest, nearest);
        }
    }
    EXPECT_LT(deepest, 1.0e-4f) << "the lid's outline leaves the ring";
    const std::vector<PavedBorderRun> borders = BuildPavedBorder(region.Runs, PavedBorderSettings{}, Flat, {});
    ASSERT_EQ(borders.size(), 1u);
    for (const SplineStripStation& station : borders[0].Stations)
        EXPECT_GE(station.HalfWidthLeft, deepest) << "a gap between the lid and the border's foot";
}
