#include "SplineGeometry/PavedBorder.h"

#include "Mathematics/Angle.h"
#include "SplineGeometry/PavedFootprint.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace GameEngine::SplineGeometry
{

using Mathematics::Vector2;
using Mathematics::Vector3;

namespace
{

const Vector3 kUp(0.0f, 1.0f, 0.0f);
// A boundary point whose run turns by more than this is a corner (SplineCornerStyle), as every
// point the boolean made is: a border swept round a sharper turn without one narrows by more than
// 1 % (cos(7.5 degrees) = 0.991).
constexpr float32 kCornerTurnDegrees = 15.0f;
// The finest station spacing a run takes, so a zero spacing cannot sample it without end.
constexpr float32 kMinBorderSpacingMeters = 0.01f;

Vector3 NormalizedOr(const Vector3& v, const Vector3& fallback)
{
    const float32 length = v.Length();
    return length > 0.0f ? v * (1.0f / length) : fallback;
}

// Whether the run turns at point `point` by more than kCornerTurnDegrees; an open run's
// ends never do.
bool TurnsSharply(std::span<const Vector2> points, std::size_t point, bool closed)
{
    const std::size_t count = points.size();
    if (!closed && (point == 0 || point + 1 >= count))
        return false;
    const Vector2 in = points[point] - points[(point + count - 1) % count];
    const Vector2 out = points[(point + 1) % count] - points[point];
    const float32 lengths = in.Length() * out.Length();
    if (!(lengths > 0.0f))
        return false;
    const float32 cosine = std::clamp(Vector2::Dot(in, out) / lengths, -1.0f, 1.0f);
    return std::acos(cosine) > Mathematics::ToRadians(kCornerTurnDegrees);
}

// A closed run started at the middle of its longest edge, so the loop's seam lies on a straight
// stretch rather than at a corner, whose rings the seam would split.
PavedBoundaryRun SeamOnLongestEdge(const PavedBoundaryRun& run)
{
    const std::size_t count = run.Points.size();
    std::size_t longest = 0;
    float32 longestSquared = -1.0f;
    for (std::size_t i = 0; i < count; ++i)
    {
        const Vector2 edge = run.Points[(i + 1) % count] - run.Points[i];
        const float32 lengthSquared = Vector2::Dot(edge, edge);
        if (lengthSquared > longestSquared)
        {
            longestSquared = lengthSquared;
            longest = i;
        }
    }
    PavedBoundaryRun seamed;
    seamed.Closed = true;
    seamed.Hole = run.Hole;
    seamed.Points.push_back((run.Points[longest] + run.Points[(longest + 1) % count]) * 0.5f);
    seamed.NewPoints.push_back(0u);
    seamed.Edges.push_back(run.Edges[longest]);
    for (std::size_t step = 1; step <= count; ++step)
    {
        const std::size_t i = (longest + step) % count;
        seamed.Points.push_back(run.Points[i]);
        seamed.NewPoints.push_back(run.NewPoints[i]);
        seamed.Edges.push_back(run.Edges[i]);
    }
    return seamed;
}

void AppendStation(std::vector<SplineStripStation>& stations, const Vector2& xz, const GroundSampler& groundAt)
{
    stations.emplace_back().Position = Vector3(xz.x, groundAt(xz), xz.y);
}

// The stations along `run`'s points at most `spacing` apart, a closed run's start reached again at
// its end, and the corner sites at its New and sharply turning points.
void SampleRun(const PavedBoundaryRun& run, float32 spacing, const GroundSampler& groundAt,
               std::vector<SplineStripStation>& stations, std::vector<SplineCornerSite>& sites)
{
    const std::span<const Vector2> points(run.Points);
    const std::size_t count = points.size();
    const std::size_t legs = run.Closed ? count : count - 1;
    for (std::size_t leg = 0; leg < legs; ++leg)
    {
        const Vector2& from = points[leg];
        const Vector2& to = points[(leg + 1) % count];
        const Vector2 along = to - from;
        const float32 length = along.Length();
        const uint32 steps = std::max(1u, static_cast<uint32>(std::ceil(length / spacing)));
        if (run.NewPoints[leg] != 0 || TurnsSharply(points, leg, run.Closed))
            sites.push_back(SplineCornerSite{static_cast<uint32>(stations.size()), static_cast<uint32>(leg)});
        for (uint32 step = 0; step < steps; ++step)
            AppendStation(stations, from + along * (static_cast<float32>(step) / static_cast<float32>(steps)), groundAt);
    }
    AppendStation(stations, points[run.Closed ? 0 : count - 1], groundAt);
}

// Gives each station its frame (forward along the chord through its neighbors, right of travel
// level, up between them), its half widths (kAreaLidInsetMeters in over the lid, `width` out) and
// its distance along the run.
void FrameStations(std::vector<SplineStripStation>& stations, float32 width)
{
    float32 distance = 0.0f;
    Vector3 forward(0.0f, 0.0f, 1.0f);
    for (std::size_t i = 0; i < stations.size(); ++i)
    {
        if (i > 0)
            distance += (stations[i].Position - stations[i - 1].Position).Length();
        const Vector3& ahead = stations[std::min(i + 1, stations.size() - 1)].Position;
        const Vector3& behind = stations[i == 0 ? 0 : i - 1].Position;
        forward = NormalizedOr(ahead - behind, forward);
        SplineStripStation& station = stations[i];
        station.Forward = forward;
        station.Right = NormalizedOr(Vector3::Cross(kUp, Vector3(forward.x, 0.0f, forward.z)), Vector3(1.0f, 0.0f, 0.0f));
        station.Up = Vector3::Cross(forward, station.Right);
        station.HalfWidthLeft = kAreaLidInsetMeters;
        station.HalfWidthRight = width;
        station.Distance = distance;
    }
}

// Sinks each station's grounded base by the ground's fall from the station to the border's outer
// base edge (along MiterRight on a mitered side) and kPavedBorderFootingMeters more, so neither a
// cross slope nor a lifted station floats it.
void SinkGroundedBase(std::vector<SplineStripStation>& stations, const GroundSampler& groundAt)
{
    for (SplineStripStation& station : stations)
    {
        const Vector3 axis =
            (station.MiterSides & kSplineMiterRightSide) != 0u ? station.MiterRight : station.Right;
        const Vector3 edge = station.Position + axis * station.HalfWidthRight;
        const float32 fall = station.Position.y - groundAt(Vector2(edge.x, edge.z));
        station.GroundOffset = -(std::max(0.0f, fall) + kPavedBorderFootingMeters);
    }
}

// The verge an open run's end meets: the arm of the Open edge `neighbor`, when its verge is On.
const PavedVerge* VergeBeyond(const PavedEdge& neighbor, std::span<const PavedVerge> armVerges)
{
    if (neighbor.Source != PavedEdgeSource::Open || neighbor.Index >= armVerges.size())
        return nullptr;
    const PavedVerge& verge = armVerges[neighbor.Index];
    return verge.On ? &verge : nullptr;
}

// The length over which the border at one end of `run` takes the verge of arm `arm`: from that end
// over the arm's side and Mouth edges to the last Mouth edge (the arc, or the straight edge to the
// road's side), else one border `width`. It stops at the middle of the run, so the two ends do not overlap.
float32 HandOffLength(const PavedBoundaryRun& run, bool atEnd, uint32 arm, float32 width)
{
    const std::size_t edges = run.Edges.size();
    float32 runLength = 0.0f;
    for (std::size_t edge = 0; edge < edges; ++edge)
        runLength += (run.Points[edge + 1u] - run.Points[edge]).Length();
    float32 walked = 0.0f;
    float32 length = 0.0f;
    for (std::size_t step = 0; step < edges; ++step)
    {
        const std::size_t edge = atEnd ? edges - 1u - step : step;
        const PavedEdge& source = run.Edges[edge];
        const bool mouth = source.Source == PavedEdgeSource::Mouth;
        if (source.Index != arm || !(mouth || source.Source == PavedEdgeSource::ArmSide))
            break;
        walked += (run.Points[edge + 1u] - run.Points[edge]).Length();
        if (mouth)
            length = walked;
    }
    return std::min(length > 0.0f ? length : width, runLength * 0.5f);
}

// Turns the station at one end of the run square to the Open edge after it, whose direction in the
// ring's winding is `along`: the edge crosses the road, so the ring lies across the road as the
// verge's first ring does. The border's right points away from the road.
void SquareToOpenEdge(SplineStripStation& station, const Vector2& along, bool atEnd)
{
    const Vector3 edge = NormalizedOr(Vector3(along.x, 0.0f, along.y), Vector3(0.0f, 0.0f, 0.0f));
    if (edge.LengthSquared() == 0.0f)
        return;
    station.Right = atEnd ? edge * -1.0f : edge;
    station.Forward = Vector3::Cross(station.Right, kUp);
    station.Up = Vector3::Cross(station.Forward, station.Right);
}

// Adds the change from the border's size to `verge`'s over `length` of the stations at one end of
// the run, so the border's last ring there is the verge's first ring. The two ends of a run add.
void TakeVergeSize(std::vector<SplineStripStation>& stations, bool atEnd, float32 length,
                   const PavedBorderSettings& settings, const PavedVerge& verge)
{
    if (!(length > 0.0f))
        return;
    const float32 endDistance = atEnd ? stations.back().Distance : stations.front().Distance;
    const float32 width = std::max(settings.Width, 0.0f);
    for (SplineStripStation& station : stations)
    {
        const float32 away = std::abs(station.Distance - endDistance);
        if (away >= length)
            continue;
        const float32 t = 1.0f - away / length;
        station.HalfWidthRight += (verge.Width - width) * t;
        station.TopOffset += (verge.Height - settings.Height) * t;
    }
}

SplineStripParams StripParams(const std::vector<SplineStripStation>& stations, bool closed)
{
    SplineStripParams params;
    params.WidthScale = SplineProfileScale::LateralOnly;
    params.CapStart = !closed;
    params.CapEnd = !closed;
    if (stations.empty())
        return params;
    params.UOriginMetres = stations.front().Distance;
    if (closed)
    {
        const SplineStripStation& last = stations.back();
        params.LoopLengthMetres = last.Distance - stations.front().Distance;
        params.LoopFaceTurnLeft = last.FaceTurnLeft;
        params.LoopFaceTurnRight = last.FaceTurnRight;
        params.LoopTopArcAllowance = last.TopArcAllowance;
    }
    return params;
}

PavedBorderRun BuildRun(const PavedBoundaryRun& run, const PavedBorderSettings& settings, const GroundSampler& groundAt,
                        std::span<const PavedVerge> armVerges)
{
    PavedBorderRun border;
    border.Closed = run.Closed;
    if (run.Points.size() < (run.Closed ? 3u : 2u))
        return border;
    std::vector<SplineCornerSite> sites;
    const float32 spacing = std::max(settings.StationSpacingMeters, kMinBorderSpacingMeters);
    SampleRun(run.Closed ? SeamOnLongestEdge(run) : run, spacing, groundAt, border.Stations, sites);
    FrameStations(border.Stations, std::max(settings.Width, 0.0f));
    if (run.Closed)
        CloseStationLoop(border.Stations, kUp);
    if (!sites.empty() && settings.Corner != SplineCornerStyle::None)
        border.CornerIssues = ApplyCorners(border.Stations, sites, settings.Corner, kUp, run.Closed);
    const PavedVerge* startVerge = run.Closed ? nullptr : VergeBeyond(run.StartBreak, armVerges);
    const PavedVerge* endVerge = run.Closed ? nullptr : VergeBeyond(run.EndBreak, armVerges);
    if (startVerge)
    {
        TakeVergeSize(border.Stations, false, HandOffLength(run, false, run.StartBreak.Index, settings.Width), settings, *startVerge);
        SquareToOpenEdge(border.Stations.front(), run.StartBreakDirection, false);
    }
    if (endVerge)
    {
        TakeVergeSize(border.Stations, true, HandOffLength(run, true, run.EndBreak.Index, settings.Width), settings, *endVerge);
        SquareToOpenEdge(border.Stations.back(), run.EndBreakDirection, true);
    }
    AccumulateFaceTurn(border.Stations, kUp);
    SinkGroundedBase(border.Stations, groundAt);
    border.Strip = StripParams(border.Stations, run.Closed);
    border.Strip.CapStart &= startVerge == nullptr;
    border.Strip.CapEnd &= endVerge == nullptr;
    return border;
}

} // namespace

SplineProfile BuildPavedBorderProfile(const PavedBorderSettings& settings)
{
    SplineProfileParams params;
    params.Shape = SplineProfileShape::Rectangle;
    params.Width = settings.Width;
    params.Height = settings.Height;
    return BuildProfile(params);
}

std::vector<PavedBorderRun> BuildPavedBorder(std::span<const PavedBoundaryRun> runs,
                                             const PavedBorderSettings& settings, const GroundSampler& groundAt,
                                             std::span<const PavedVerge> armVerges)
{
    std::vector<PavedBorderRun> borders;
    borders.reserve(runs.size());
    for (const PavedBoundaryRun& run : runs)
        borders.push_back(BuildRun(run, settings, groundAt, armVerges));
    return borders;
}

} // namespace GameEngine::SplineGeometry
