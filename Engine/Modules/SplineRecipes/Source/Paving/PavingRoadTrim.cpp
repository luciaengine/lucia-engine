#include "SplineRecipes/Paving/PavingRoadTrim.h"

#include "Mathematics/Interpolation.h"

#include <algorithm>
#include <cmath>

namespace GameEngine::SplineRecipes
{
namespace
{

namespace SG = GameEngine::SplineGeometry;
using Mathematics::Vector3;

// A kept run or a skipped interval shorter than this, in meters, is dropped: a sliver of strip.
constexpr float32 kMinTrimMeters = 0.01f;

// Arc length a road's strip skips, from one trim or several that overlap. A cap is the trim whose
// Open edge lies at that end: at High the outward travel ascends, at Low it descends.
struct SkippedInterval
{
    float32 Low = 0.0f;
    float32 High = 0.0f;
    const PavingRoadTrim* LowCap = nullptr;
    const PavingRoadTrim* HighCap = nullptr;
};

// The intervals `trims` skip, sorted and merged; on a closed road each lies in [0, roadLoopMeters],
// one that crosses the seam split in two.
std::vector<SkippedInterval> SkippedIntervals(std::span<const PavingRoadTrim> trims, float32 roadLoopMeters)
{
    std::vector<SkippedInterval> intervals;
    for (const PavingRoadTrim& trim : trims)
    {
        float32 delta = trim.StopDistance - trim.NodeDistance;
        if (roadLoopMeters > 0.0f)
            delta = std::remainder(delta, roadLoopMeters);
        if (std::abs(delta) < kMinTrimMeters)
            continue;
        SkippedInterval interval;
        interval.Low = delta > 0.0f ? trim.NodeDistance : trim.NodeDistance + delta;
        if (trim.Open)
            (delta > 0.0f ? interval.HighCap : interval.LowCap) = &trim;
        if (roadLoopMeters > 0.0f)
        {
            interval.Low = std::fmod(interval.Low, roadLoopMeters);
            if (interval.Low < 0.0f)
                interval.Low += roadLoopMeters;
        }
        interval.High = interval.Low + std::abs(delta);
        if (roadLoopMeters > 0.0f && interval.High > roadLoopMeters)
        {
            intervals.push_back({interval.Low, roadLoopMeters, interval.LowCap, nullptr});
            intervals.push_back({0.0f, interval.High - roadLoopMeters, nullptr, interval.HighCap});
            continue;
        }
        intervals.push_back(interval);
    }
    std::sort(intervals.begin(), intervals.end(),
              [](const SkippedInterval& a, const SkippedInterval& b) { return a.Low < b.Low; });

    std::vector<SkippedInterval> merged;
    for (const SkippedInterval& interval : intervals)
    {
        if (merged.empty() || interval.Low > merged.back().High)
        {
            merged.push_back(interval);
            continue;
        }
        SkippedInterval& last = merged.back();
        if (interval.High > last.High || (interval.High == last.High && !last.HighCap))
        {
            last.High = interval.High;
            last.HighCap = interval.HighCap;
        }
    }
    return merged;
}

SG::SplineStripStation Interpolate(const SG::SplineStripStation& a, const SG::SplineStripStation& b, float32 t)
{
    SG::SplineStripStation station = a;
    station.Position = a.Position + (b.Position - a.Position) * t;
    station.Right = (a.Right + (b.Right - a.Right) * t).NormalizeOrZero();
    station.Up = (a.Up + (b.Up - a.Up) * t).NormalizeOrZero();
    station.Forward = (a.Forward + (b.Forward - a.Forward) * t).NormalizeOrZero();
    station.HalfWidthLeft = a.HalfWidthLeft + (b.HalfWidthLeft - a.HalfWidthLeft) * t;
    station.HalfWidthRight = a.HalfWidthRight + (b.HalfWidthRight - a.HalfWidthRight) * t;
    station.RollRadians = a.RollRadians + (b.RollRadians - a.RollRadians) * t;
    station.Distance = a.Distance + (b.Distance - a.Distance) * t;
    return station;
}

// `along` moved onto the Open edge of `trim`: square across the road, the edge at the region's
// heights. Its half-widths are those of `along`, measured on the plan and stretched along the tilt.
// The tilt rides in Right, so the roll is zero.
SG::SplineStripStation OpenEdgeStation(const SG::SplineStripStation& along, const PavingRoadTrim& trim,
                                       bool outwardAscends, const Mathematics::Matrix4x4& invPlacerWorld)
{
    const Vector3 left = invPlacerWorld.TransformPoint(outwardAscends ? trim.OpenLeft : trim.OpenRight);
    const Vector3 right = invPlacerWorld.TransformPoint(outwardAscends ? trim.OpenRight : trim.OpenLeft);
    const Vector3 across = right - left;
    const float32 halfWidth = 0.5f * across.Length();
    if (!(halfWidth > 0.0f))
        return along;
    const float32 planHalfWidth = 0.5f * (across - along.Up * Vector3::Dot(across, along.Up)).Length();
    const float32 stretch = planHalfWidth > 0.0f ? halfWidth / planHalfWidth : 1.0f;
    SG::SplineStripStation station = along;
    station.HalfWidthLeft = along.HalfWidthLeft * stretch;
    station.HalfWidthRight = along.HalfWidthRight * stretch;
    station.Position = (left + right) * 0.5f;
    station.Right = across * (0.5f / halfWidth);
    const Vector3 forward =
        (along.Forward - station.Right * Vector3::Dot(along.Forward, station.Right)).NormalizeOrZero();
    if (forward.LengthSquared() > 0.0f)
        station.Forward = forward;
    station.Up = Vector3::Cross(station.Forward, station.Right).NormalizeOrZero();
    station.RollRadians = 0.0f;
    return station;
}

// The Open edge of `trim` in placer space, left and right of a run's travel: a run that starts on it
// travels outward (`outwardAscends`), one that ends on it travels inward.
TrimmedRunEdge RunEdgeOn(const PavingRoadTrim* trim, bool outwardAscends, const Mathematics::Matrix4x4& invPlacerWorld)
{
    TrimmedRunEdge edge;
    if (!trim)
        return edge;
    edge.Open = true;
    edge.Curbed = trim->Curbed;
    edge.Left = invPlacerWorld.TransformPoint(outwardAscends ? trim->OpenLeft : trim->OpenRight);
    edge.Right = invPlacerWorld.TransformPoint(outwardAscends ? trim->OpenRight : trim->OpenLeft);
    return edge;
}

// The stations along ascending arc length: the input, and on a closed road a second lap after it,
// so a run can cross the seam.
struct UnrolledStations
{
    std::vector<SG::SplineStripStation> Stations;
    std::vector<float32> RoadDistance;
    std::vector<uint32> Source;
};

UnrolledStations Unroll(std::span<const SG::SplineStripStation> stations, std::span<const float32> roadDistance,
                        float32 roadLoopMeters)
{
    UnrolledStations out;
    const uint32 laps = roadLoopMeters > 0.0f ? 2u : 1u;
    const float32 worldLap = stations.back().Distance - stations.front().Distance;
    for (uint32 lap = 0; lap < laps; ++lap)
    {
        for (uint32 i = 0; i < stations.size(); ++i)
        {
            const float32 distance = roadDistance[i] + roadLoopMeters * static_cast<float32>(lap);
            if (!out.RoadDistance.empty() && distance <= out.RoadDistance.back())
                continue;
            SG::SplineStripStation station = stations[i];
            station.Distance += worldLap * static_cast<float32>(lap);
            out.Stations.push_back(station);
            out.RoadDistance.push_back(distance);
            out.Source.push_back(i);
        }
    }
    return out;
}

// The station at arc length `distance`, and the input station it lies on or after.
std::pair<SG::SplineStripStation, uint32> StationAt(const UnrolledStations& road, float32 distance)
{
    const auto upper = std::upper_bound(road.RoadDistance.begin(), road.RoadDistance.end(), distance);
    const size_t high = std::clamp<size_t>(static_cast<size_t>(upper - road.RoadDistance.begin()), 1u,
                                           road.RoadDistance.size() - 1u);
    const size_t low = high - 1u;
    const float32 span = road.RoadDistance[high] - road.RoadDistance[low];
    const float32 t = span > 0.0f ? Math::Clamp01((distance - road.RoadDistance[low]) / span) : 0.0f;
    return {Interpolate(road.Stations[low], road.Stations[high], t), road.Source[low]};
}

// Eases the crown of the run `stations` to flat at each Open cap, by a smooth step over one road width
// from the cap, so the strip's top meets the region's flat lid along the whole cap. Near two caps, a
// station takes the lower of the two eases.
void EaseRiseToOpenCaps(std::span<SG::SplineStripStation> stations, bool lowCap, bool highCap)
{
    for (const bool atHigh : {false, true})
    {
        if (!(atHigh ? highCap : lowCap))
            continue;
        const SG::SplineStripStation& cap = atHigh ? stations.back() : stations.front();
        const float32 length = cap.HalfWidthLeft + cap.HalfWidthRight;
        for (SG::SplineStripStation& station : stations)
        {
            const float32 away = std::abs(station.Distance - cap.Distance);
            const float32 rise = length > 0.0f ? Math::SmoothStep(0.0f, length, away) : (away > 0.0f ? 1.0f : 0.0f);
            station.RiseScale = std::min(station.RiseScale, rise);
        }
    }
}

// Appends the run of `road` from arc length `low` to `high` to `out`, its ends on the Open edges
// of `lowCap` and `highCap` when they are set, its crown eased to flat at each.
void AppendRun(const UnrolledStations& road, float32 low, float32 high, const PavingRoadTrim* lowCap,
               const PavingRoadTrim* highCap, const Mathematics::Matrix4x4& invPlacerWorld,
               TrimmedSweepStations& out)
{
    if (high - low < kMinTrimMeters)
        return;
    const uint32 first = static_cast<uint32>(out.Stations.size());
    const auto [start, startSource] = StationAt(road, low);
    out.Stations.push_back(lowCap ? OpenEdgeStation(start, *lowCap, true, invPlacerWorld) : start);
    out.Source.push_back(startSource);
    for (uint32 i = 0; i < road.Stations.size(); ++i)
    {
        if (road.RoadDistance[i] > low + kMinTrimMeters && road.RoadDistance[i] < high - kMinTrimMeters)
        {
            out.Stations.push_back(road.Stations[i]);
            out.Source.push_back(road.Source[i]);
        }
    }
    const auto [end, endSource] = StationAt(road, high);
    out.Stations.push_back(highCap ? OpenEdgeStation(end, *highCap, false, invPlacerWorld) : end);
    out.Source.push_back(endSource);
    EaseRiseToOpenCaps(std::span(out.Stations).subspan(first), lowCap != nullptr, highCap != nullptr);
    out.Runs.emplace_back(first, static_cast<uint32>(out.Stations.size()) - 1u);
    out.RunEdges.emplace_back(RunEdgeOn(lowCap, true, invPlacerWorld), RunEdgeOn(highCap, false, invPlacerWorld));
}

bool SamePoint(const Vector3& a, const Vector3& b)
{
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

} // namespace

bool PavingRoadTrim::operator==(const PavingRoadTrim& other) const
{
    return NodeDistance == other.NodeDistance && StopDistance == other.StopDistance && Open == other.Open &&
           SamePoint(OpenLeft, other.OpenLeft) && SamePoint(OpenRight, other.OpenRight) && Curbed == other.Curbed;
}

float32 RoadArcScale(const Mathematics::Matrix4x4& worldMatrix)
{
    const Vector3 origin = worldMatrix.TransformPoint({0.0f, 0.0f, 0.0f});
    const float32 x = (worldMatrix.TransformPoint({1.0f, 0.0f, 0.0f}) - origin).Length();
    const float32 z = (worldMatrix.TransformPoint({0.0f, 0.0f, 1.0f}) - origin).Length();
    return std::max(x, z);
}

TrimmedSweepStations TrimSweepStations(std::span<const SG::SplineStripStation> stations,
                                       std::span<const float32> roadDistance, float32 roadLoopMeters,
                                       std::span<const PavingRoadTrim> trims,
                                       const Mathematics::Matrix4x4& invPlacerWorld)
{
    TrimmedSweepStations out;
    if (stations.size() < 2u || roadDistance.size() != stations.size())
        return out;
    const UnrolledStations road = Unroll(stations, roadDistance, roadLoopMeters);
    if (road.Stations.size() < 2u)
        return out;
    const std::vector<SkippedInterval> skipped = SkippedIntervals(trims, roadLoopMeters);

    if (roadLoopMeters > 0.0f)
    {
        if (skipped.empty())
        {
            AppendRun(road, road.RoadDistance.front(), roadLoopMeters, nullptr, nullptr, invPlacerWorld, out);
            return out;
        }
        for (size_t k = 0; k < skipped.size(); ++k)
        {
            const bool wraps = k + 1u == skipped.size();
            const SkippedInterval& next = skipped[wraps ? 0u : k + 1u];
            AppendRun(road, skipped[k].High, next.Low + (wraps ? roadLoopMeters : 0.0f), skipped[k].HighCap,
                      next.LowCap, invPlacerWorld, out);
        }
        return out;
    }

    float32 cursor = road.RoadDistance.front();
    const PavingRoadTrim* cursorCap = nullptr;
    for (const SkippedInterval& interval : skipped)
    {
        AppendRun(road, cursor, std::min(interval.Low, road.RoadDistance.back()), cursorCap, interval.LowCap,
                  invPlacerWorld, out);
        if (interval.High > cursor)
        {
            cursor = interval.High;
            cursorCap = interval.HighCap;
        }
    }
    AppendRun(road, cursor, road.RoadDistance.back(), cursorCap, nullptr, invPlacerWorld, out);
    return out;
}

} // namespace GameEngine::SplineRecipes
