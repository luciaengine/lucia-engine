#include "MarkupECS/MarkupRegionMesh.h"

#include "MarkupECS/MarkupRegionOutline.h"
#include "Mathematics/Geometry.h"
#include "Mathematics/Triangulation.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <unordered_map>

namespace GameEngine::MarkupECS
{

using Mathematics::Vector2;
using Mathematics::Vector3;

namespace
{

// Outline samples closer than this are one sample.
constexpr float32 kSameSampleMeters = 1.0e-3f;
constexpr uint32 kNoTriangle = std::numeric_limits<uint32>::max();

Vector2 GroundXZ(const Vector3& point) { return Vector2(point.x, point.z); }

float32 LengthSquared(const Vector2& v) { return v.x * v.x + v.y * v.y; }

// The samples with near-duplicates merged, and a closed ring's repeated first point dropped.
std::vector<Vector3> MergeSamples(std::span<const Vector3> outline, bool closed)
{
    std::vector<Vector3> merged;
    merged.reserve(outline.size());
    for (const Vector3& point : outline)
    {
        if (!merged.empty() && LengthSquared(GroundXZ(point) - GroundXZ(merged.back())) <=
                                   kSameSampleMeters * kSameSampleMeters)
            continue;
        merged.push_back(point);
    }
    while (closed && merged.size() > 1 &&
           LengthSquared(GroundXZ(merged.back()) - GroundXZ(merged.front())) <= kSameSampleMeters * kSameSampleMeters)
        merged.pop_back();
    return merged;
}

// The lid's triangulation on the ground plane, relative to the outline's first point (so a
// region far from the origin keeps its precision): the ring ear-clipped, then every edge longer
// than the limit split at its midpoint in both triangles that share it, so no triangle meets
// another at a T and the raised lid has no cracks.
class LidRefiner
{
  public:
    // `points` are the ring's samples (the first `ringCount`, at their `heights`, in ring order),
    // `triangles` the ear-clipped kept corners.
    LidRefiner(std::vector<Vector2> points, std::size_t ringCount, std::vector<uint32> triangles,
               std::vector<float32> heights)
        : m_Points(std::move(points)), m_Heights(std::move(heights)), m_RingCount(ringCount)
    {
        m_RingPosition.resize(m_Points.size());
        for (std::size_t i = 0; i < m_Points.size(); ++i)
            m_RingPosition[i] = i < ringCount ? static_cast<float32>(i) : kInterior;
        for (std::size_t t = 0; t + 2 < triangles.size(); t += 3)
        {
            const uint32 index = static_cast<uint32>(m_Triangles.size());
            m_Triangles.push_back({triangles[t], triangles[t + 1], triangles[t + 2]});
            for (int k = 0; k < 3; ++k)
                AddEdge(triangles[t + static_cast<std::size_t>(k)], triangles[t + static_cast<std::size_t>((k + 1) % 3)],
                        index);
        }
    }

    // Splits until no edge is longer than `maxEdge`, or the triangle cap is reached, by
    // longest-edge propagation (Rivara's LEPP): a triangle's longest edge is split only once it is
    // the longest edge of the triangle across it too, after that neighbour has been split along
    // its own. So the split never cuts a short edge of a sliver, and the triangle count stays near
    // the area over the limit squared.
    void Refine(float32 maxEdge)
    {
        const float32 limitSquared = maxEdge * maxEdge;
        for (std::size_t t = 0; t < m_Triangles.size() && m_Triangles.size() < kMaxLidTriangles; ++t)
        {
            while (m_Triangles.size() < kMaxLidTriangles)
            {
                const std::pair<uint32, uint32> edge = LongestEdge(static_cast<uint32>(t));
                if (LengthSquared(m_Points[edge.second] - m_Points[edge.first]) <= limitSquared)
                    break;
                const std::pair<uint32, uint32> terminal = TerminalEdge(static_cast<uint32>(t), edge);
                SplitEdge(terminal.first, terminal.second);
            }
        }
    }

    // Splits every outline edge at the ring samples it spans (the collinear ones left out of the ear
    // clip), so the lid's edge passes through every sample the walls stand on and the two meet
    // without a crack: the triangle on such an edge becomes a fan from its opposite corner (and a
    // fan triangle that still holds another such edge, at a corner of the area, fans again).
    void InsertSkippedSamples()
    {
        std::vector<std::array<uint32, 3>> refined;
        refined.reserve(m_Triangles.size() * 2);
        for (const std::array<uint32, 3>& triangle : m_Triangles)
            FanSkippedSamples(triangle, refined);
        m_Triangles = std::move(refined);
    }

    const std::vector<Vector2>& GetPoints() const { return m_Points; }
    // NaN for a point inside the area, which takes the ground sampler's height.
    const std::vector<float32>& GetHeights() const { return m_Heights; }
    std::vector<uint32> FlattenTriangles() const
    {
        std::vector<uint32> flat;
        flat.reserve(m_Triangles.size() * 3);
        for (const std::array<uint32, 3>& triangle : m_Triangles)
            flat.insert(flat.end(), triangle.begin(), triangle.end());
        return flat;
    }

  private:
    // A triangle's longest edge; ties go to the smaller edge key, so the triangles either side
    // of an edge agree on whether it is their longest.
    std::pair<uint32, uint32> LongestEdge(uint32 triangle) const
    {
        const std::array<uint32, 3>& corners = m_Triangles[triangle];
        std::pair<uint32, uint32> longest{corners[0], corners[1]};
        float32 longestSquared = -1.0f;
        for (std::size_t k = 0; k < 3; ++k)
        {
            const uint32 a = corners[k];
            const uint32 b = corners[(k + 1) % 3];
            const float32 lengthSquared = LengthSquared(m_Points[b] - m_Points[a]);
            if (lengthSquared > longestSquared ||
                (lengthSquared == longestSquared && Key(a, b) < Key(longest.first, longest.second)))
            {
                longestSquared = lengthSquared;
                longest = {a, b};
            }
        }
        return longest;
    }

    // The end of the longest-edge path from `triangle`: across each longest edge to the neighbour
    // while the neighbour's longest edge is another one; the edge where the path stops is the
    // longest edge of both triangles that share it (or lies on the outline).
    std::pair<uint32, uint32> TerminalEdge(uint32 triangle, std::pair<uint32, uint32> edge) const
    {
        for (std::size_t step = 0; step < m_Triangles.size(); ++step)
        {
            const auto found = m_Edges.find(Key(edge.first, edge.second));
            if (found == m_Edges.end())
                return edge;
            const uint32 neighbour = found->second[0] == triangle ? found->second[1] : found->second[0];
            if (neighbour == kNoTriangle)
                return edge;
            const std::pair<uint32, uint32> next = LongestEdge(neighbour);
            if (Key(next.first, next.second) == Key(edge.first, edge.second))
                return edge;
            triangle = neighbour;
            edge = next;
        }
        return edge;
    }

    // The ring position of `point`, the midpoint of outline edge (u, v): the sample segment between
    // their positions it lies on, and how far along it.
    float32 RingPositionOn(uint32 u, uint32 v, const Vector2& point) const
    {
        float32 from = m_RingPosition[u];
        float32 to = m_RingPosition[v];
        if (to < from)
            std::swap(from, to);
        // The edge runs the short way round the ring.
        if (to - from > 0.5f * static_cast<float32>(m_RingCount))
        {
            std::swap(from, to);
            to += static_cast<float32>(m_RingCount);
        }
        float32 best = from;
        float32 bestDistance = std::numeric_limits<float32>::max();
        for (float32 start = std::floor(from); start < to; start += 1.0f)
        {
            const Vector2& a = m_Points[static_cast<std::size_t>(start) % m_RingCount];
            const Vector2& b = m_Points[(static_cast<std::size_t>(start) + 1) % m_RingCount];
            const Vector2 ab = b - a;
            const float32 lengthSquared = LengthSquared(ab);
            const float32 s = lengthSquared > 0.0f
                                  ? std::clamp(((point.x - a.x) * ab.x + (point.y - a.y) * ab.y) / lengthSquared, 0.0f, 1.0f)
                                  : 0.0f;
            const float32 distance = LengthSquared(a + ab * s - point);
            if (distance < bestDistance)
            {
                bestDistance = distance;
                best = std::fmod(start + s, static_cast<float32>(m_RingCount));
            }
        }
        return best;
    }

    // The ring's height at a ring position, between its two samples.
    float32 RingHeightAt(float32 position) const
    {
        const std::size_t a = static_cast<std::size_t>(position) % m_RingCount;
        const std::size_t b = (a + 1) % m_RingCount;
        const float32 s = position - std::floor(position);
        return m_Heights[a] + (m_Heights[b] - m_Heights[a]) * s;
    }

    void FanSkippedSamples(const std::array<uint32, 3>& triangle, std::vector<std::array<uint32, 3>>& out) const
    {
        std::vector<uint32> samples;
        for (std::size_t k = 0; k < 3; ++k)
        {
            const uint32 a = triangle[k];
            const uint32 b = triangle[(k + 1) % 3];
            const uint32 c = triangle[(k + 2) % 3];
            samples.clear();
            AppendSkippedSamples(a, b, samples);
            if (samples.empty())
                continue;
            uint32 previous = a;
            samples.push_back(b);
            for (const uint32 sample : samples)
            {
                FanSkippedSamples({previous, sample, c}, out);
                previous = sample;
            }
            return;
        }
        out.push_back(triangle);
    }

    // The ring samples strictly between outline edge (a, b)'s ends, in ring order, appended to
    // `out`; none for an edge inside the area.
    void AppendSkippedSamples(uint32 a, uint32 b, std::vector<uint32>& out) const
    {
        if (m_RingPosition[a] == kInterior || m_RingPosition[b] == kInterior)
            return;
        const auto found = m_Edges.find(Key(a, b));
        if (found == m_Edges.end() || found->second[1] != kNoTriangle)
            return;
        // The edge spans the short way round the ring, in either direction.
        const float32 ring = static_cast<float32>(m_RingCount);
        const float32 forward = std::fmod(m_RingPosition[b] - m_RingPosition[a] + ring, ring);
        const bool reversed = forward > 0.5f * ring;
        const float32 from = reversed ? m_RingPosition[b] : m_RingPosition[a];
        const float32 to = from + (reversed ? ring - forward : forward);
        const std::size_t first = out.size();
        for (float32 sample = std::floor(from) + 1.0f; sample < to; sample += 1.0f)
            out.push_back(static_cast<uint32>(static_cast<std::size_t>(sample) % m_RingCount));
        if (reversed)
            std::reverse(out.begin() + static_cast<std::ptrdiff_t>(first), out.end());
    }

    static uint64_t Key(uint32 a, uint32 b)
    {
        return (static_cast<uint64_t>(std::min(a, b)) << 32) | std::max(a, b);
    }

    void AddEdge(uint32 a, uint32 b, uint32 triangle)
    {
        auto [it, inserted] = m_Edges.try_emplace(Key(a, b), std::array<uint32, 2>{kNoTriangle, kNoTriangle});
        (it->second[0] == kNoTriangle ? it->second[0] : it->second[1]) = triangle;
    }

    void ReplaceEdgeTriangle(uint32 a, uint32 b, uint32 from, uint32 to)
    {
        std::array<uint32, 2>& sides = m_Edges[Key(a, b)];
        (sides[0] == from ? sides[0] : sides[1]) = to;
    }

    // Splits edge (u, v) at its midpoint in every triangle that has it; a midpoint on the outline
    // (an edge with one triangle) takes the height on the line between its ends, so the lid's
    // edge stays on the walls' top edge.
    void SplitEdge(uint32 u, uint32 v)
    {
        const auto found = m_Edges.find(Key(u, v));
        if (found == m_Edges.end())
            return;
        const std::array<uint32, 2> sides = found->second;
        m_Edges.erase(found);
        const bool onOutline = sides[1] == kNoTriangle;
        const uint32 middle = static_cast<uint32>(m_Points.size());
        const Vector2 point = (m_Points[u] + m_Points[v]) * 0.5f;
        m_Points.push_back(point);
        if (onOutline)
        {
            // On the outline the point takes the ring's own height there, so the lid's edge stays on
            // the walls' top edge.
            const float32 position = RingPositionOn(u, v, point);
            m_RingPosition.push_back(position);
            m_Heights.push_back(RingHeightAt(position));
        }
        else
        {
            m_RingPosition.push_back(kInterior);
            m_Heights.push_back(std::numeric_limits<float32>::quiet_NaN());
        }
        for (const uint32 t : sides)
        {
            if (t == kNoTriangle)
                continue;
            const std::array<uint32, 3> triangle = m_Triangles[t];
            int k = 0;
            while (k < 3 && !((triangle[static_cast<std::size_t>(k)] == u && triangle[static_cast<std::size_t>((k + 1) % 3)] == v) ||
                              (triangle[static_cast<std::size_t>(k)] == v && triangle[static_cast<std::size_t>((k + 1) % 3)] == u)))
                ++k;
            const uint32 a = triangle[static_cast<std::size_t>(k)];
            const uint32 b = triangle[static_cast<std::size_t>((k + 1) % 3)];
            const uint32 c = triangle[static_cast<std::size_t>((k + 2) % 3)];
            const uint32 added = static_cast<uint32>(m_Triangles.size());
            m_Triangles[t] = {a, middle, c};
            m_Triangles.push_back({middle, b, c});
            ReplaceEdgeTriangle(b, c, t, added);
            AddEdge(a, middle, t);
            AddEdge(middle, b, added);
            AddEdge(middle, c, t);
            AddEdge(middle, c, added);
        }
    }

    // A point inside the area has no ring position.
    static constexpr float32 kInterior = -1.0f;

    std::vector<Vector2> m_Points;
    std::vector<float32> m_Heights;
    std::size_t m_RingCount = 0;
    std::vector<float32> m_RingPosition; // a sample's index, or the place between two
    std::vector<std::array<uint32, 3>> m_Triangles;
    std::unordered_map<uint64_t, std::array<uint32, 2>> m_Edges;
};

// The ring's corners: the samples a straight run between two kept samples would pass farther than
// kCornerTolerance from, so every sample left out lies on an edge between corners.
constexpr float32 kCornerTolerance = 1.0e-2f;

std::vector<uint32> RingCorners(const std::vector<Vector2>& ring)
{
    std::vector<uint32> corners{0};
    const std::size_t count = ring.size();
    std::size_t anchor = 0;
    while (anchor < count)
    {
        // The farthest sample `end` such that every sample between `anchor` and it lies on the
        // straight run between the two.
        std::size_t end = anchor + 1;
        while (end < count)
        {
            const std::size_t next = end + 1;
            const Vector2& a = ring[anchor];
            const Vector2& b = ring[next % count];
            const Vector2 ab = b - a;
            const float32 length = std::sqrt(LengthSquared(ab));
            bool straight = length > 0.0f;
            for (std::size_t k = anchor + 1; straight && k < next; ++k)
            {
                const Vector2 ak = ring[k] - a;
                straight = std::abs(ab.x * ak.y - ab.y * ak.x) / length <= kCornerTolerance;
            }
            if (!straight)
                break;
            end = next;
        }
        if (end >= count)
            break;
        corners.push_back(static_cast<uint32>(end));
        anchor = end;
    }
    return corners;
}

// The lid over the closed, simple ring `ring` (counter-clockwise from above).
void BuildLid(const std::vector<Vector3>& ring, const MarkupGroundSampler& groundAt, MarkupRegionGround& ground)
{
    const Vector2 origin = GroundXZ(ring.front());
    std::vector<Vector2> local;
    std::vector<float32> heights;
    local.reserve(ring.size());
    heights.reserve(ring.size());
    Vector2 min = GroundXZ(ring.front());
    Vector2 max = min;
    for (const Vector3& point : ring)
    {
        local.push_back(GroundXZ(point) - origin);
        heights.push_back(point.y);
        min = Vector2(std::min(min.x, point.x), std::min(min.y, point.z));
        max = Vector2(std::max(max.x, point.x), std::max(max.y, point.z));
    }
    // The ear clip takes the ring's corners only: a straight run of samples would clip into flat
    // slivers the split could never make well-shaped. The samples left out go back into the
    // outline edges after the split.
    std::vector<uint32> triangles;
    Mathematics::EarClip(local, RingCorners(local), triangles);

    const std::size_t ringCount = local.size();
    LidRefiner refiner(std::move(local), ringCount, std::move(triangles), std::move(heights));
    const float32 extent = std::max(max.x - min.x, max.y - min.y);
    refiner.Refine(std::max(kLidMinEdgeMeters, extent / kLidEdgeExtentDivisions));
    refiner.InsertSkippedSamples();

    std::vector<Vector2> ring2D;
    ring2D.reserve(ring.size());
    for (const Vector3& point : ring)
        ring2D.push_back(GroundXZ(point) - origin);
    const std::vector<Vector2>& points = refiner.GetPoints();
    const std::vector<float32>& pointHeights = refiner.GetHeights();
    ground.LidPoints.reserve(points.size());
    ground.LidEdgeDistance.reserve(points.size());
    for (std::size_t i = 0; i < points.size(); ++i)
    {
        const Vector2 world = points[i] + origin;
        const float32 height = std::isnan(pointHeights[i]) ? groundAt(world) : pointHeights[i];
        ground.LidPoints.emplace_back(world.x, height, world.y);
        // The ring's own points and the outline's midpoints lie on it.
        ground.LidEdgeDistance.push_back(std::isnan(pointHeights[i]) ? RegionOutlineDistance(points[i], ring2D) : 0.0f);
    }
    ground.LidTriangles = refiner.FlattenTriangles();
}

// The point halfway along an open polyline's length.
Vector3 HalfwayAlong(const std::vector<Vector3>& line)
{
    float32 remaining = 0.5f * PolylineLength(line);
    for (std::size_t i = 1; i < line.size(); ++i)
    {
        const float32 segment = (line[i] - line[i - 1]).Length();
        if (segment >= remaining && segment > 0.0f)
            return line[i - 1] + (line[i] - line[i - 1]) * (remaining / segment);
        remaining -= segment;
    }
    return line.back();
}

void AppendVertex(MarkupRegionMesh& mesh, const Vector3& position, float32 rim)
{
    mesh.Vertices.push_back(MarkupRegionVertex{position, rim});
}

// Reads the ground across a path's band at every sample (EdgeGround).
void SamplePathEdges(MarkupRegionGround& ground, const MarkupGroundSampler& groundAt)
{
    const std::vector<Vector3>& line = ground.Outline;
    ground.EdgeGround.reserve(line.size() * kPathEdgeOffsets.size() * 2);
    for (std::size_t i = 0; i < line.size(); ++i)
    {
        const Vector3 across = PathAcross(line, i);
        for (const float32 side : {-1.0f, 1.0f})
        {
            for (const float32 offset : kPathEdgeOffsets)
                ground.EdgeGround.push_back(groundAt(GroundXZ(line[i] + across * (side * offset))));
        }
    }
}

} // namespace

float32 PolylineLength(std::span<const Vector3> line)
{
    float32 length = 0.0f;
    for (std::size_t i = 1; i < line.size(); ++i)
        length += (line[i] - line[i - 1]).Length();
    return length;
}

Vector3 PathAcross(std::span<const Vector3> line, std::size_t i)
{
    const Vector3& previous = line[i == 0 ? 0 : i - 1];
    const Vector3& next = line[std::min(i + 1, line.size() - 1)];
    const Vector3 along = next - previous;
    return Vector3(along.z, 0.0f, -along.x).NormalizeOrZero();
}

float32 PathEdgeHeight(const MarkupRegionGround& ground, std::size_t i, float32 side, float32 halfWidth)
{
    constexpr std::size_t kOffsets = kPathEdgeOffsets.size();
    if (ground.EdgeGround.size() != ground.Outline.size() * kOffsets * 2)
        return ground.Outline[i].y;
    const float32* heights = ground.EdgeGround.data() + (i * 2 + (side > 0.0f ? 1 : 0)) * kOffsets;
    if (halfWidth <= kPathEdgeOffsets.front())
        return heights[0];
    for (std::size_t k = 1; k < kOffsets; ++k)
    {
        if (halfWidth <= kPathEdgeOffsets[k])
        {
            const float32 t = (halfWidth - kPathEdgeOffsets[k - 1]) / (kPathEdgeOffsets[k] - kPathEdgeOffsets[k - 1]);
            return heights[k - 1] + (heights[k] - heights[k - 1]) * t;
        }
    }
    return heights[kOffsets - 1];
}

MarkupRegionGround BuildMarkupRegionGround(std::span<const Vector3> outline, bool closed,
                                           const MarkupGroundSampler& groundAt)
{
    MarkupRegionGround ground;
    ground.Outline = MergeSamples(outline, closed);
    if (ground.Outline.empty())
        return ground;

    Vector3 min = ground.Outline.front();
    Vector3 max = min;
    for (const Vector3& point : ground.Outline)
    {
        min = Vector3(std::min(min.x, point.x), std::min(min.y, point.y), std::min(min.z, point.z));
        max = Vector3(std::max(max.x, point.x), std::max(max.y, point.y), std::max(max.z, point.z));
    }
    ground.Center = (min + max) * 0.5f;
    ground.Top = max.y;
    for (const Vector3& point : ground.Outline)
        ground.Radius = std::max(ground.Radius, (point - ground.Center).Length());

    ground.Closed = closed && ground.Outline.size() >= kMinRegionKnots;
    std::vector<Vector2>& ring2D = ground.Ring;
    ring2D.reserve(ground.Outline.size());
    for (const Vector3& point : ground.Outline)
        ring2D.push_back(GroundXZ(point));
    if (!ground.Closed)
    {
        ground.LabelGround = HalfwayAlong(ground.Outline);
        if (!closed)
            SamplePathEdges(ground, groundAt);
        return ground;
    }
    if (Mathematics::PolygonDoubledSignedArea(ring2D) < 0.0f)
    {
        std::reverse(ground.Outline.begin(), ground.Outline.end());
        std::reverse(ring2D.begin(), ring2D.end());
    }
    const Vector2 label = RegionLabelPoint(ring2D);
    ground.LabelGround = Vector3(label.x, groundAt(label), label.y);
    // The drape caps the samples at kMaxRegionKnots, so the outline rule's count bound holds; a
    // crossing, or a sliver with no area, has no lid.
    ground.Crosses = CheckRegionOutline(ring2D).has_value();
    if (!ground.Crosses)
        BuildLid(ground.Outline, groundAt, ground);
    return ground;
}

MarkupRegionMesh BuildMarkupRegionMesh(const MarkupRegionGround& ground, float32 extrudeHeight)
{
    MarkupRegionMesh mesh;
    const Vector3 lift(0.0f, extrudeHeight, 0.0f);
    mesh.Label = ground.LabelGround + lift;
    const std::vector<Vector3>& outline = ground.Outline;
    if (outline.empty())
        return mesh;
    mesh.BaseOutline = outline;
    if (!ground.Closed)
        return mesh;
    mesh.BaseOutline.push_back(outline.front());
    mesh.TopOutline.reserve(mesh.BaseOutline.size());
    for (const Vector3& point : mesh.BaseOutline)
        mesh.TopOutline.push_back(point + lift);

    // The rim coordinate is the distance to the top edge over the rim's width.
    const float32 rimWidth = std::max(kMarkupRegionRimWidthFraction * extrudeHeight, 1.0e-3f);
    const float32 bottomRim = extrudeHeight / rimWidth;
    mesh.Vertices.reserve(outline.size() * 6 + ground.LidTriangles.size());
    for (std::size_t i = 0; i < outline.size(); ++i)
    {
        // Quad from sample i to sample i + 1, wound to face out of the counter-clockwise ring:
        // seen from outside each triangle turns counter-clockwise, as markup_glow.vert winds a
        // box's faces.
        const Vector3& from = outline[i];
        const Vector3& to = outline[(i + 1) % outline.size()];
        AppendVertex(mesh, to, bottomRim);
        AppendVertex(mesh, from + lift, 0.0f);
        AppendVertex(mesh, from, bottomRim);
        AppendVertex(mesh, to, bottomRim);
        AppendVertex(mesh, to + lift, 0.0f);
        AppendVertex(mesh, from + lift, 0.0f);
    }
    mesh.WallVertexCount = mesh.Vertices.size();
    for (const uint32 index : ground.LidTriangles)
        AppendVertex(mesh, ground.LidPoints[index] + lift, ground.LidEdgeDistance[index] / rimWidth);
    return mesh;
}

} // namespace GameEngine::MarkupECS
