#include "RoadVerge.h"

#include "Components/Spline/SplineExtrude.h"
#include "SplineGeometry/AreaGround.h"
#include "SplineGeometry/PavedBorder.h"
#include "SplineRecipes/Paving/PavingRoadTrim.h"
#include "SplineRecipes/SplineExtrudeController.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace GameEngine::SplineRecipes
{
namespace
{

namespace SG = GameEngine::SplineGeometry;
using Mathematics::Vector3;

enum class VergeSide : uint8
{
    Left,
    Right,
};

// How far the road's surface falls below the top's edge under the verge: the edge drop, and on a
// Crown the shoulder's drop too.
float32 EdgeFall(const Components::SplineExtrude& recipe)
{
    const float32 drop = std::max(recipe.EdgeDrop, 0.0f);
    if (recipe.Profile == Components::SplineExtrudeProfile::Crown)
        return std::max(drop, std::max(recipe.ShoulderDrop, 0.0f));
    return drop;
}

// The verge's station beside `station` on `side`: on the edge of the strip's top, or on the Open
// edge's end when `edge` is set, with the curb's half widths mirrored so its outside faces away from
// the road. The curb keeps its whole height where the road eases its crown.
SG::SplineStripStation VergeStation(const Components::SplineExtrude& recipe, const SG::SplineStripStation& station,
                                    VergeSide side, const TrimmedRunEdge* edge, float32 groundOffset)
{
    SG::SplineStripStation verge = station;
    const bool left = side == VergeSide::Left;
    if (edge && edge->Open)
    {
        verge.Position = left ? edge->Left : edge->Right;
    }
    else
    {
        // The strip's top edge rolls with the builder's basis.
        const Vector3 rolledRight = SG::RolledRight(station);
        const float32 top = SplineExtrudeController::StripTopHalfWidth(
            recipe, left ? station.HalfWidthLeft : station.HalfWidthRight);
        verge.Position = station.Position + rolledRight * (left ? -top : top);
    }
    verge.HalfWidthLeft = left ? recipe.VergeWidth : SG::kAreaLidInsetMeters;
    verge.HalfWidthRight = left ? SG::kAreaLidInsetMeters : recipe.VergeWidth;
    verge.MiterSides = 0;
    verge.GroundOffset = groundOffset;
    verge.TopOffset = 0.0f;
    verge.RiseScale = 1.0f;
    return verge;
}

} // namespace

void AppendStripMesh(const SG::SplineStripMesh& strip, SG::SplineStripMesh& out)
{
    if (!strip.IsValid())
        return;
    const uint32 base = static_cast<uint32>(out.Vertices.size());
    if (out.Vertices.empty())
    {
        out.MinBounds = strip.MinBounds;
        out.MaxBounds = strip.MaxBounds;
    }
    out.MinBounds = Vector3(std::min(out.MinBounds.x, strip.MinBounds.x), std::min(out.MinBounds.y, strip.MinBounds.y),
                            std::min(out.MinBounds.z, strip.MinBounds.z));
    out.MaxBounds = Vector3(std::max(out.MaxBounds.x, strip.MaxBounds.x), std::max(out.MaxBounds.y, strip.MaxBounds.y),
                            std::max(out.MaxBounds.z, strip.MaxBounds.z));
    out.Vertices.insert(out.Vertices.end(), strip.Vertices.begin(), strip.Vertices.end());
    for (const uint32 index : strip.Indices)
        out.Indices.push_back(base + index);
}

SG::SplineStripMesh BuildRoadVerge(const Components::SplineExtrude& recipe, std::span<const SG::SplineStripStation> road,
                                   const RoadVergeEnd& start, const RoadVergeEnd& end, float32 uOrigin)
{
    SG::SplineStripMesh mesh;
    if (road.size() < 2u)
        return mesh;
    SG::PavedBorderSettings curb;
    curb.Width = recipe.VergeWidth;
    curb.Height = recipe.VergeHeight;
    const SG::SplineProfile profile = SG::BuildPavedBorderProfile(curb);
    const float32 groundOffset = -(EdgeFall(recipe) + SG::kPavedBorderFootingMeters);

    SG::SplineStripParams params;
    params.WidthScale = SG::SplineProfileScale::LateralOnly;
    params.UOriginMetres = uOrigin;
    params.CapStart = start.Capped;
    params.CapEnd = end.Capped;

    std::vector<SG::SplineStripStation> stations(road.size());
    for (const VergeSide side : {VergeSide::Left, VergeSide::Right})
    {
        for (size_t i = 0; i < road.size(); ++i)
        {
            const TrimmedRunEdge* edge = i == 0 ? start.Edge : (i + 1u == road.size() ? end.Edge : nullptr);
            stations[i] = VergeStation(recipe, road[i], side, edge, groundOffset);
        }
        AppendStripMesh(SG::BuildSplineStrip(profile, stations, params), mesh);
    }
    return mesh;
}

} // namespace GameEngine::SplineRecipes
