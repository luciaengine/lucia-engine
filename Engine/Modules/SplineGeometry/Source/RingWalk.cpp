#include "RingWalk.h"

#include "Mathematics/Geometry.h"
#include "SplineGeometry/PavedFootprint.h"

#include <cmath>
#include <limits>

namespace GameEngine::SplineGeometry
{

using Mathematics::Vector2;

RingWalk::RingWalk(std::span<const Vector2> ring) : m_Ring(ring), m_Start(ring.size() + 1, 0.0f)
{
    for (std::size_t i = 0; i < ring.size(); ++i)
        m_Start[i + 1] = m_Start[i] + (ring[(i + 1) % ring.size()] - ring[i]).Length();
}

RingSpot RingWalk::Nearest(const Vector2& point) const
{
    RingSpot best;
    float32 bestDistance = std::numeric_limits<float32>::max();
    for (std::size_t i = 0; i < m_Ring.size(); ++i)
    {
        const Vector2 closest = Mathematics::ClosestPointOnSegment(point, m_Ring[i], m_Ring[(i + 1) % m_Ring.size()]);
        const float32 distance = (closest - point).Length();
        if (distance < bestDistance)
        {
            bestDistance = distance;
            best = RingSpot{closest, m_Start[i] + (closest - m_Ring[i]).Length(), i};
        }
    }
    return best;
}

std::optional<RingSpot> RingWalk::FirstCrossing(const Vector2& from, const Vector2& to) const
{
    std::optional<RingSpot> first;
    float32 nearest = std::numeric_limits<float32>::max();
    for (std::size_t i = 0; i < m_Ring.size(); ++i)
    {
        const Vector2& a = m_Ring[i];
        const Vector2& b = m_Ring[(i + 1) % m_Ring.size()];
        const std::optional<Vector2> fractions = Mathematics::SegmentIntersectionFractions(from, to, a, b);
        if (!fractions || fractions->x >= nearest)
            continue;
        nearest = fractions->x;
        first = RingSpot{Mathematics::Lerp(a, b, fractions->y), m_Start[i] + (b - a).Length() * fractions->y, i};
    }
    return first;
}

float32 RingWalk::SignedDistance(const Vector2& point) const
{
    const float32 distance = (Nearest(point).Point - point).Length();
    return Mathematics::PointInPolygon(point, m_Ring) ? -distance : distance;
}

Vector2 RingWalk::Tangent(const RingSpot& spot) const
{
    return (m_Ring[(spot.Edge + 1) % m_Ring.size()] - m_Ring[spot.Edge]).Normalize();
}

float32 RingWalk::Along(float32 from, float32 to, int direction) const
{
    const float32 perimeter = GetPerimeter();
    const float32 along = std::fmod((to - from) * static_cast<float32>(direction), perimeter);
    return along < 0.0f ? along + perimeter : along;
}

void RingWalk::AppendBetween(const RingSpot& from, const RingSpot& to, int direction, AreaRing& out) const
{
    const std::size_t count = m_Ring.size();
    const float32 total = Along(from.Length, to.Length, direction);
    std::size_t point = direction > 0 ? (from.Edge + 1) % count : from.Edge;
    for (std::size_t step = 0; step < count; ++step)
    {
        const float32 along = Along(from.Length, m_Start[point], direction);
        if (along >= total - kPavedTouchMeters)
            return;
        if (along > kPavedTouchMeters)
            out.push_back(m_Ring[point]);
        point = direction > 0 ? (point + 1) % count : (point + count - 1) % count;
    }
}

} // namespace GameEngine::SplineGeometry
