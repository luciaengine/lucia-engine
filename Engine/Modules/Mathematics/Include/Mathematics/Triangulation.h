#pragma once

#include "Mathematics/Vector2.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace GameEngine::Mathematics
{

// How EarClip tells a degenerate corner from a real one, in the polygon's own units.
struct EarClipTolerances
{
    // Twice the area below which an ear is degenerate (a corner that runs straight on).
    float MinDoubledArea = 1.0e-12f;
    // Points closer than this are one point: they never block an ear.
    float SamePointDistance = 1.0e-5f;
};

namespace EarClipDetail
{

inline float Cross2(const Vector2& a, const Vector2& b) { return a.x * b.y - a.y * b.x; }

// Strictly inside a counter-clockwise triangle: a point on an edge is not.
inline bool StrictlyInsideTriangle(const Vector2& a, const Vector2& b, const Vector2& c, const Vector2& p,
                                   float minDoubledArea)
{
    return Cross2(b - a, p - a) > minDoubledArea && Cross2(c - b, p - b) > minDoubledArea &&
           Cross2(a - c, p - c) > minDoubledArea;
}

inline bool SamePoint(const Vector2& a, const Vector2& b, float distance)
{
    const Vector2 d = a - b;
    return d.x * d.x + d.y * d.y <= distance * distance;
}

// Whether the corner at polygon[i] is an ear: turning left by more than `minTurn` with no other
// polygon point strictly inside the triangle it cuts.
inline bool IsEar(std::span<const Vector2> points, const std::vector<std::uint32_t>& polygon, std::size_t i,
                  float minTurn, const EarClipTolerances& tolerances)
{
    const std::size_t count = polygon.size();
    const Vector2& a = points[polygon[(i + count - 1u) % count]];
    const Vector2& b = points[polygon[i]];
    const Vector2& c = points[polygon[(i + 1u) % count]];
    if (!(Cross2(b - a, c - b) > minTurn))
        return false;
    for (std::size_t k = 0; k < count; ++k)
    {
        const Vector2& p = points[polygon[k]];
        if (SamePoint(p, a, tolerances.SamePointDistance) || SamePoint(p, b, tolerances.SamePointDistance) ||
            SamePoint(p, c, tolerances.SamePointDistance))
            continue;
        if (StrictlyInsideTriangle(a, b, c, p, tolerances.MinDoubledArea))
            return false;
    }
    return true;
}

} // namespace EarClipDetail

// Ear-clips the counter-clockwise polygon `polygon` (indices into `points`, +y up) into
// counter-clockwise triangles appended to `out` as index triples: a polygon of n corners gives
// n - 2 triangles. The engine's one polygon triangulator.
//
// A corner where the outline runs straight on is clipped as a flat triangle once no true ear
// remains, rather than dropped, so every outline edge stays an edge of a triangle. A polygon
// that folds over itself (no ear clips) has its first corner clipped anyway so the loop ends; the
// fold has no area to cover. A hole is spliced in by the caller as a zero-width bridge. O(n^3)
// at worst: meant for outlines of a few hundred corners.
inline void EarClip(std::span<const Vector2> points, std::vector<std::uint32_t> polygon,
                    std::vector<std::uint32_t>& out, const EarClipTolerances& tolerances = {})
{
    while (polygon.size() > 3u)
    {
        const std::size_t count = polygon.size();
        std::size_t ear = count;
        for (const float minTurn : {tolerances.MinDoubledArea, -tolerances.MinDoubledArea})
        {
            for (std::size_t i = 0; i < count && ear == count; ++i)
            {
                if (EarClipDetail::IsEar(points, polygon, i, minTurn, tolerances))
                    ear = i;
            }
            if (ear != count)
                break;
        }
        if (ear == count)
            ear = 0;
        out.insert(out.end(), {polygon[(ear + count - 1u) % count], polygon[ear], polygon[(ear + 1u) % count]});
        polygon.erase(polygon.begin() + static_cast<std::ptrdiff_t>(ear));
    }
    if (polygon.size() == 3u)
        out.insert(out.end(), {polygon[0], polygon[1], polygon[2]});
}

} // namespace GameEngine::Mathematics
