#pragma once

#include "SplineGeometry/SplineStation.h"
#include "SplineGeometry/SplineStripBuilder.h"
#include "Types/Types.h"

#include <span>

namespace GameEngine::Components
{
struct SplineExtrude;
}

namespace GameEngine::SplineRecipes
{

struct TrimmedRunEdge;

// One end of a stretch of a road's verge.
struct RoadVergeEnd
{
    // The stretch ends here with a cap: the end of the road, or an Open edge that no curb reaches.
    bool Capped = false;
    // The Open edge the end lies on, or null: the verge's ring there stands on the edge's ends, where
    // the paving's curb ends.
    const TrimmedRunEdge* Edge = nullptr;
};

// The verge of `recipe` along the road stations `road`, in placer space: on each side, a sweep of the
// paving curb's cross-section that stands on the edge of the strip's top, VergeWidth wide and VergeHeight
// high. Its base sinks the road's edge drop and the curb's footing. U is in meters from `uOrigin`.
SplineGeometry::SplineStripMesh BuildRoadVerge(const Components::SplineExtrude& recipe,
                                               std::span<const SplineGeometry::SplineStripStation> road,
                                               const RoadVergeEnd& start, const RoadVergeEnd& end, float32 uOrigin);

// Appends `strip` to `out`: its vertices, its indices moved past `out`'s vertices, and its bounds.
void AppendStripMesh(const SplineGeometry::SplineStripMesh& strip, SplineGeometry::SplineStripMesh& out);

} // namespace GameEngine::SplineRecipes
