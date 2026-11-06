#pragma once

#include "Mathematics/Vector2.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>
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

// Whether side ab of the counter-clockwise triangles (a, b, c) and (b, a, d) flips to side cd: the
// angles at c and d sum to more than a half turn, and (a, d, c) and (d, b, c) are both wider than
// `minDoubledArea`. A clockwise triangle (a folded outline) never flips.
inline bool ShouldFlipForDelaunay(const Vector2& a, const Vector2& b, const Vector2& c, const Vector2& d,
                                  float minDoubledArea)
{
    // The margin, relative to the four sides' lengths, keeps a flip from undoing another at four
    // points on one circle.
    constexpr double kRelativeMargin = 1.0e-9;
    // Each coordinate widens before the subtraction, so the test gives one answer for the quad
    // read from either side.
    const double ax = a.x, ay = a.y, bx = b.x, by = b.y, cx = c.x, cy = c.y, dx = d.x, dy = d.y;
    const double cax = ax - cx, cay = ay - cy, cbx = bx - cx, cby = by - cy;
    const double dbx = bx - dx, dby = by - dy, dax = ax - dx, day = ay - dy;
    const double crossC = cax * cby - cay * cbx;
    const double crossD = dbx * day - dby * dax;
    if (crossC < -minDoubledArea || crossD < -minDoubledArea)
        return false;
    // The doubled areas of (a, d, c) and (d, b, c).
    const double dcx = dax - cax, dcy = day - cay;
    if (!(dax * cay - day * cax > minDoubledArea) || !(dbx * dcy - dby * dcx > minDoubledArea))
        return false;
    const double dotC = cax * cbx + cay * cby;
    const double dotD = dbx * dax + dby * day;
    const double scale = std::sqrt((cax * cax + cay * cay) * (cbx * cbx + cby * cby) * (dax * dax + day * day) *
                                   (dbx * dbx + dby * dby));
    // sin(C + D) times the four side lengths: below zero when C + D is more than a half turn.
    return crossC * dotD + dotC * crossD < -kRelativeMargin * scale;
}

// The neighbor across a triangle side that no other triangle shares (an outline side).
inline constexpr std::uint32_t kNoTriangle = std::numeric_limits<std::uint32_t>::max();
// The neighbor across a side that more than two triangles share, or that two triangles run in the
// same direction (the outline folds there).
inline constexpr std::uint32_t kManyTriangles = kNoTriangle - 1u;

namespace TriangleLinkDetail
{

// The side of `corners` from `p` to `q`, either way round, with `neighbor` across it.
inline std::uint32_t SideToward(const std::array<std::uint32_t, 3>& corners,
                                const std::array<std::uint32_t, 3>& neighbors, std::uint32_t neighbor,
                                std::uint32_t p, std::uint32_t q)
{
    std::uint32_t side = 0;
    while (side < 2u && !(neighbors[side] == neighbor &&
                          ((corners[side] == p && corners[(side + 1u) % 3u] == q) ||
                           (corners[side] == q && corners[(side + 1u) % 3u] == p))))
        ++side;
    return side;
}

// Points the side of `triangle` from `p` to `q` at `to` in place of `from`.
inline void Relink(std::span<const std::array<std::uint32_t, 3>> triangles,
                   std::span<std::array<std::uint32_t, 3>> neighbors, std::uint32_t triangle, std::uint32_t from,
                   std::uint32_t p, std::uint32_t q, std::uint32_t to)
{
    if (triangle < kManyTriangles)
        neighbors[triangle][SideToward(triangles[triangle], neighbors[triangle], from, p, q)] = to;
}

} // namespace TriangleLinkDetail

// The triangle across each side of `triangles` (index triples): neighbors[t][k] lies across side k
// of triangle t, from its corner k to the next; else kNoTriangle or kManyTriangles.
inline std::vector<std::array<std::uint32_t, 3>>
LinkTriangles(std::span<const std::array<std::uint32_t, 3>> triangles)
{
    struct SideRecord
    {
        std::uint64_t Key = 0;
        std::uint32_t Triangle = 0;
        std::uint32_t Side = 0;
    };
    std::vector<SideRecord> records;
    records.reserve(triangles.size() * 3u);
    for (std::uint32_t t = 0; t < triangles.size(); ++t)
    {
        for (std::uint32_t k = 0; k < 3u; ++k)
        {
            const std::uint32_t from = triangles[t][k];
            const std::uint32_t to = triangles[t][(k + 1u) % 3u];
            records.push_back({(static_cast<std::uint64_t>(std::min(from, to)) << 32u) | std::max(from, to), t, k});
        }
    }
    std::sort(records.begin(), records.end(),
              [](const SideRecord& a, const SideRecord& b) { return a.Key < b.Key; });
    std::vector<std::array<std::uint32_t, 3>> neighbors(triangles.size(), {kNoTriangle, kNoTriangle, kNoTriangle});
    for (std::size_t first = 0, end = 0; first < records.size(); first = end)
    {
        end = first + 1u;
        while (end < records.size() && records[end].Key == records[first].Key)
            ++end;
        if (end - first == 1u)
            continue;
        const SideRecord& a = records[first];
        const SideRecord& b = records[first + 1u];
        // Two triangles that run the side in opposite directions lie on its two sides.
        if (end - first == 2u && a.Triangle != b.Triangle &&
            triangles[a.Triangle][a.Side] != triangles[b.Triangle][b.Side])
        {
            neighbors[a.Triangle][a.Side] = b.Triangle;
            neighbors[b.Triangle][b.Side] = a.Triangle;
            continue;
        }
        for (std::size_t r = first; r < end; ++r)
            neighbors[records[r].Triangle][records[r].Side] = kManyTriangles;
    }
    return neighbors;
}

// Flips each side in `pending` (a triangle and its side) that breaks the Delaunay rule, and then
// the four outer sides of each flipped pair, until none breaks it (Lawson's flips). A side flips
// only between two triangles that form a convex quad, so the points and the outline stay.
inline void FlipToDelaunay(std::span<const Vector2> points, std::span<std::array<std::uint32_t, 3>> triangles,
                           std::span<std::array<std::uint32_t, 3>> neighbors,
                           std::vector<std::pair<std::uint32_t, std::uint32_t>>& pending, float minDoubledArea)
{
    while (!pending.empty())
    {
        const auto [triangle, side] = pending.back();
        pending.pop_back();
        const std::uint32_t neighbor = neighbors[triangle][side];
        if (neighbor >= kManyTriangles)
            continue;
        // Triangle `triangle` is (a, b, c) from `side`; triangle `neighbor` is (b, a, d).
        const std::array<std::uint32_t, 3> corners = triangles[triangle];
        const std::uint32_t a = corners[side];
        const std::uint32_t b = corners[(side + 1u) % 3u];
        const std::uint32_t c = corners[(side + 2u) % 3u];
        const std::uint32_t across =
            TriangleLinkDetail::SideToward(triangles[neighbor], neighbors[neighbor], triangle, a, b);
        const std::uint32_t d = triangles[neighbor][(across + 2u) % 3u];
        if (!ShouldFlipForDelaunay(points[a], points[b], points[c], points[d], minDoubledArea))
            continue;
        const std::uint32_t acrossBC = neighbors[triangle][(side + 1u) % 3u];
        const std::uint32_t acrossCA = neighbors[triangle][(side + 2u) % 3u];
        const std::uint32_t acrossAD = neighbors[neighbor][(across + 1u) % 3u];
        const std::uint32_t acrossDB = neighbors[neighbor][(across + 2u) % 3u];
        triangles[triangle] = {a, d, c};
        triangles[neighbor] = {d, b, c};
        neighbors[triangle] = {acrossAD, neighbor, acrossCA};
        neighbors[neighbor] = {acrossDB, acrossBC, triangle};
        TriangleLinkDetail::Relink(triangles, neighbors, acrossAD, neighbor, a, d, triangle);
        TriangleLinkDetail::Relink(triangles, neighbors, acrossBC, triangle, b, c, neighbor);
        pending.insert(pending.end(), {{triangle, 0u}, {triangle, 2u}, {neighbor, 0u}, {neighbor, 1u}});
    }
}

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

// On the open segment ac, its ends left out: within `minDoubledArea` of its line and between them.
inline bool OnOpenSegment(const Vector2& a, const Vector2& c, const Vector2& p, float minDoubledArea)
{
    const Vector2 ac = c - a;
    const Vector2 ap = p - a;
    const float along = ac.x * ap.x + ac.y * ap.y;
    return std::abs(Cross2(ac, ap)) <= minDoubledArea && along > 0.0f && along < ac.x * ac.x + ac.y * ac.y;
}

// Whether the corner at polygon[i] is an ear: turning left by more than `minTurn` with no other
// polygon point strictly inside the triangle it cuts or on the diagonal it cuts along. A corner on
// the diagonal (where two straight runs of the outline line up) would leave the rest of the
// outline running back over itself along it, and a later ear would then cross the outline.
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
        if (StrictlyInsideTriangle(a, b, c, p, tolerances.MinDoubledArea) ||
            OnOpenSegment(a, c, p, tolerances.MinDoubledArea))
            return false;
    }
    return true;
}

// Flips every inner side of the index triples `triangles` to the Delaunay rule.
inline void FlipAllToDelaunay(std::span<const Vector2> points, std::span<std::uint32_t> triangles,
                              float minDoubledArea)
{
    std::vector<std::array<std::uint32_t, 3>> corners(triangles.size() / 3u);
    for (std::size_t t = 0; t < corners.size(); ++t)
        corners[t] = {triangles[3u * t], triangles[3u * t + 1u], triangles[3u * t + 2u]};
    std::vector<std::array<std::uint32_t, 3>> neighbors = LinkTriangles(corners);
    std::vector<std::pair<std::uint32_t, std::uint32_t>> pending;
    for (std::uint32_t t = 0; t < corners.size(); ++t)
    {
        for (std::uint32_t k = 0; k < 3u; ++k)
        {
            if (neighbors[t][k] < kManyTriangles && t < neighbors[t][k])
                pending.emplace_back(t, k);
        }
    }
    FlipToDelaunay(points, corners, neighbors, pending, minDoubledArea);
    for (std::size_t t = 0; t < corners.size(); ++t)
        std::copy(corners[t].begin(), corners[t].end(), triangles.begin() + static_cast<std::ptrdiff_t>(3u * t));
}

} // namespace EarClipDetail

// Ear-clips the counter-clockwise polygon `polygon` (indices into `points`, +y up, holes bridged in
// by BridgeHole) into n - 2 counter-clockwise triangles appended to `out`, flipped to Delaunay. A
// straight-on corner stays a flat triangle, so every outline side stays. O(n^3) at worst.
inline void EarClip(std::span<const Vector2> points, std::vector<std::uint32_t> polygon,
                    std::vector<std::uint32_t>& out, const EarClipTolerances& tolerances = {})
{
    const std::size_t first = out.size();
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
        // A folded outline has no ear: its first corner clips so that the loop ends.
        if (ear == count)
            ear = 0;
        out.insert(out.end(), {polygon[(ear + count - 1u) % count], polygon[ear], polygon[(ear + 1u) % count]});
        polygon.erase(polygon.begin() + static_cast<std::ptrdiff_t>(ear));
    }
    if (polygon.size() == 3u)
        out.insert(out.end(), {polygon[0], polygon[1], polygon[2]});
    EarClipDetail::FlipAllToDelaunay(points, std::span<std::uint32_t>(out).subspan(first),
                                     tolerances.MinDoubledArea);
}

// Splices the clockwise `hole` (indices into `points`) into the counter-clockwise `outline` it
// lies inside as a zero-width bridge, through the outline corner the hole's rightmost corner can
// see (Eberly's bridge), so EarClip triangulates the outline around the hole. Bridge several
// holes rightmost first. A hole no ray to its right meets the outline from is left out.
inline void BridgeHole(std::span<const Vector2> points, std::vector<std::uint32_t>& outline,
                       std::span<const std::uint32_t> hole, const EarClipTolerances& tolerances = {})
{
    std::size_t holeStart = 0;
    for (std::size_t i = 1; i < hole.size(); ++i)
    {
        if (points[hole[i]].x > points[hole[holeStart]].x)
            holeStart = i;
    }
    const Vector2 m = points[hole[holeStart]];

    // The nearest outline edge the ray from M towards +x crosses.
    float nearestX = std::numeric_limits<float>::max();
    std::size_t hitEdge = outline.size();
    for (std::size_t i = 0; i < outline.size(); ++i)
    {
        const Vector2& a = points[outline[i]];
        const Vector2& b = points[outline[(i + 1u) % outline.size()]];
        const bool crosses = (a.y <= m.y && b.y >= m.y) || (a.y >= m.y && b.y <= m.y);
        if (!crosses || a.y == b.y)
            continue;
        const float x = a.x + (m.y - a.y) * (b.x - a.x) / (b.y - a.y);
        if (x >= m.x && x < nearestX)
        {
            nearestX = x;
            hitEdge = i;
        }
    }
    if (hitEdge == outline.size())
        return;

    const std::size_t edgeA = hitEdge;
    const std::size_t edgeB = (hitEdge + 1u) % outline.size();
    std::size_t visible = points[outline[edgeA]].x > points[outline[edgeB]].x ? edgeA : edgeB;
    const Vector2 hit(nearestX, m.y);
    // A reflex outline corner inside the triangle M, hit, candidate blocks the view; the one
    // nearest the ray's direction is then the visible one.
    const Vector2 candidate = points[outline[visible]];
    float bestAngle = std::numeric_limits<float>::max();
    for (std::size_t i = 0; i < outline.size(); ++i)
    {
        if (i == visible)
            continue;
        const Vector2& p = points[outline[i]];
        const Vector2& previous = points[outline[(i + outline.size() - 1u) % outline.size()]];
        const Vector2& next = points[outline[(i + 1u) % outline.size()]];
        const bool reflex = EarClipDetail::Cross2(p - previous, next - p) <= 0.0f;
        if (!reflex)
            continue;
        if (!EarClipDetail::StrictlyInsideTriangle(m, hit, candidate, p, tolerances.MinDoubledArea) &&
            !EarClipDetail::StrictlyInsideTriangle(m, candidate, hit, p, tolerances.MinDoubledArea))
            continue;
        const Vector2 toP = p - m;
        const float angle = std::abs(std::atan2(toP.y, toP.x));
        if (angle < bestAngle)
        {
            bestAngle = angle;
            visible = i;
        }
    }

    std::vector<std::uint32_t> spliced;
    spliced.reserve(outline.size() + hole.size() + 2u);
    spliced.insert(spliced.end(), outline.begin(), outline.begin() + static_cast<std::ptrdiff_t>(visible) + 1);
    for (std::size_t k = 0; k <= hole.size(); ++k)
        spliced.push_back(hole[(holeStart + k) % hole.size()]);
    spliced.push_back(outline[visible]);
    spliced.insert(spliced.end(), outline.begin() + static_cast<std::ptrdiff_t>(visible) + 1, outline.end());
    outline = std::move(spliced);
}

} // namespace GameEngine::Mathematics
