#include "SplineRecipes/Paving/PavingApproach.h"

#include "Mathematics/Geometry.h"
#include "Mathematics/Interpolation.h"
#include "Mathematics/Vector2.h"
#include "SplineGeometry/PavedFootprint.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace GameEngine::SplineRecipes
{
namespace
{

namespace SG = GameEngine::SplineGeometry;
using Mathematics::Vector2;
using Mathematics::Vector3;

// Signed arc length from `from` to `to` along a road; on a closed road the shorter way round.
float32 ArcDelta(float32 from, float32 to, float32 roadLoopMeters)
{
    const float32 delta = to - from;
    return roadLoopMeters > 0.0f ? std::remainder(delta, roadLoopMeters) : delta;
}

float32 WiderSide(const SG::PavedArmStation& station, float32 edgeWidth)
{
    return std::max(station.HalfWidthLeft, station.HalfWidthRight) + edgeWidth;
}

void Append(PavingApproach& approach, const Vector2& xz, float32 halfWidth, const SG::GroundSampler& ground)
{
    approach.Points.push_back(Vector3(xz.x, ground(xz), xz.y));
    approach.HalfWidths.push_back(halfWidth);
}

// Starts `approach` where it last crosses `outline`, and empties it when its stop lies inside.
void KeepPastOutline(PavingApproach& approach, std::span<const Vector2> outline)
{
    std::vector<Vector3>& points = approach.Points;
    if (points.size() < 2 || outline.size() < 3)
        return;
    if (Mathematics::PointInPolygon(Mathematics::GroundPoint(points.back()), outline))
    {
        points.clear();
        approach.HalfWidths.clear();
        return;
    }
    for (std::size_t i = points.size() - 1; i-- > 0;)
    {
        const Vector2 from = Mathematics::GroundPoint(points[i]);
        const Vector2 to = Mathematics::GroundPoint(points[i + 1]);
        float32 leaves = -1.0f;
        for (std::size_t e = 0; e < outline.size(); ++e)
        {
            const auto hit =
                Mathematics::SegmentIntersectionFractions(from, to, outline[e], outline[(e + 1) % outline.size()]);
            if (hit)
                leaves = std::max(leaves, hit->x);
        }
        if (leaves < 0.0f)
            continue;
        points[i] = points[i] + (points[i + 1] - points[i]) * leaves;
        approach.HalfWidths[i] = Math::Lerp(approach.HalfWidths[i], approach.HalfWidths[i + 1], leaves);
        points.erase(points.begin(), points.begin() + static_cast<std::ptrdiff_t>(i));
        approach.HalfWidths.erase(approach.HalfWidths.begin(), approach.HalfWidths.begin() + static_cast<std::ptrdiff_t>(i));
        return;
    }
}

} // namespace

bool PavingApproach::operator==(const PavingApproach& other) const
{
    if (!(Owner == other.Owner) || Road != other.Road || HalfWidths != other.HalfWidths
        || Points.size() != other.Points.size())
        return false;
    // memcmp takes no null pointer, even for zero bytes.
    return Points.empty() || std::memcmp(Points.data(), other.Points.data(), Points.size() * sizeof(Vector3)) == 0;
}

PavingApproach BuildPavingApproach(const SG::PavedArm& arm, const SG::PavedArmStop& stop, float32 edgeWidth,
                                   const SG::GroundSampler& ground, std::span<const Vector2> outline)
{
    PavingApproach approach;
    approach.Road = arm.Road;
    const float32 toStop = ArcDelta(stop.NodeDistance, stop.StopDistance, arm.RoadLoopMeters);
    if (arm.Stations.empty() || toStop == 0.0f)
        return approach;
    // Meters traveled outward from the node, positive toward the stop.
    const float32 outward = toStop > 0.0f ? 1.0f : -1.0f;
    const float32 stopTravel = std::abs(toStop);

    Append(approach, arm.Node, WiderSide(arm.Stations.front(), edgeWidth), ground);
    float32 previousTravel = 0.0f;
    Vector2 previous = arm.Node;
    float32 previousWidth = WiderSide(arm.Stations.front(), edgeWidth);
    for (const SG::PavedArmStation& station : arm.Stations)
    {
        const float32 travel = outward * ArcDelta(stop.NodeDistance, station.Distance, arm.RoadLoopMeters);
        if (travel <= previousTravel)
            continue;
        if (travel >= stopTravel)
        {
            const float32 t = Math::InverseLerp(previousTravel, travel, stopTravel);
            Append(approach, previous + (station.Position - previous) * t,
                   Math::Lerp(previousWidth, WiderSide(station, edgeWidth), t), ground);
            break;
        }
        Append(approach, station.Position, WiderSide(station, edgeWidth), ground);
        previousTravel = travel;
        previous = station.Position;
        previousWidth = WiderSide(station, edgeWidth);
    }
    KeepPastOutline(approach, outline);
    return approach;
}

} // namespace GameEngine::SplineRecipes
