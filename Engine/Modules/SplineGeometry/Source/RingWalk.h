#pragma once

#include "Mathematics/Vector2.h"
#include "SplineGeometry/AreaBoolean.h"
#include "Types/Types.h"

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace GameEngine::SplineGeometry
{

// A point on a closed ring: where it is and its length along the ring from the ring's first point.
struct RingSpot
{
    Mathematics::Vector2 Point{};
    float32 Length = 0.0f;
    std::size_t Edge = 0;
};

// A closed ring measured along its edges.
class RingWalk
{
  public:
    explicit RingWalk(std::span<const Mathematics::Vector2> ring);

    float32 GetPerimeter() const { return m_Start.back(); }

    RingSpot Nearest(const Mathematics::Vector2& point) const;

    // Where the segment from `from` to `to` first meets the ring, nearest `from`; none when it
    // does not meet it.
    std::optional<RingSpot> FirstCrossing(const Mathematics::Vector2& from, const Mathematics::Vector2& to) const;

    // Meters from `point` to the ring, negative inside it.
    float32 SignedDistance(const Mathematics::Vector2& point) const;

    // The direction of the ring's edge at `spot`, in the ring's order.
    Mathematics::Vector2 Tangent(const RingSpot& spot) const;

    // Meters along the ring from length `from` to length `to`, going in the ring's order when
    // `direction` is 1 and against it when -1.
    float32 Along(float32 from, float32 to, int direction) const;

    // The ring's points strictly between `from` and `to`, going `direction`, appended to `out`.
    void AppendBetween(const RingSpot& from, const RingSpot& to, int direction, AreaRing& out) const;

  private:
    std::span<const Mathematics::Vector2> m_Ring;
    std::vector<float32> m_Start;
};

} // namespace GameEngine::SplineGeometry
