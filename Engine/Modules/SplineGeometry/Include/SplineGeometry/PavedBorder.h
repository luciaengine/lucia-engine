#pragma once

#include "SplineGeometry/AreaGround.h"
#include "SplineGeometry/SplineCorner.h"
#include "SplineGeometry/SplineProfile.h"
#include "SplineGeometry/SplineStation.h"
#include "SplineGeometry/SplineStripBuilder.h"
#include "Types/Types.h"

#include <span>
#include <vector>

namespace GameEngine::SplineGeometry
{

struct PavedBoundaryRun;

// Meters the border's base reaches below the ground under it: a footing, so a lid lifted off the
// ground or a coarse far terrain opens no gap under the border.
inline constexpr float32 kPavedBorderFootingMeters = 0.25f;

// The border a paved region's boundary runs carry.
struct PavedBorderSettings
{
    // Meters across the border, out from the boundary, and above the lid.
    float32 Width = 0.3f;
    float32 Height = 0.12f;
    // The turn at every point the boolean made and every corner of the boundary.
    SplineCornerStyle Corner = SplineCornerStyle::Miter;
    // Stations along a run at most this far apart, so the border follows the ground: the drape's
    // step.
    float32 StationSpacingMeters = 0.5f;
};

// The verge of the road on one arm (Components::SplineExtrude::Verge): a curb of the border's
// cross-section along the road, in meters. An open run meets it on the arm's Open edge.
struct PavedVerge
{
    bool On = false;
    float32 Width = 0.0f;
    float32 Height = 0.0f;
};

// One run's border, ready for BuildSplineStrip with BuildPavedBorderProfile.
struct PavedBorderRun
{
    // In world meters. Each stands on the boundary at the ground's height there; the border runs
    // from kAreaLidInsetMeters to its left, over the lid's edge, to Width to the right of travel,
    // out of the paving (into the hole on a hole's run), and stands Height above it, its base sunk
    // kPavedBorderFootingMeters below the ground under its outer edge.
    std::vector<SplineStripStation> Stations;
    // U along the run in world meters from its first station; a closed run's faces each span a
    // whole number of tiles; an open run is capped square to its last edges at each end that meets
    // no verge.
    SplineStripParams Strip;
    // The corners the sweep could not build as asked (ApplyCorners).
    std::vector<SplineCornerIssue> CornerIssues;
    bool Closed = false;
};

// The border's cross-section: a Rectangle `settings.Width` wide and `settings.Height` tall whose
// base points are Grounded, placed by the stations' per-side half widths (kAreaLidInsetMeters to
// the left, Width to the right).
SplineProfile BuildPavedBorderProfile(const PavedBorderSettings& settings);

// The border of each run (PavedFootprint), sampled at most StationSpacingMeters apart on `groundAt`, with
// the Corner style at each turn. A run end on the Open edge of an arm whose verge in `armVerges` is On has
// no cap, and takes the verge's size over the mouth's edges, else over its last Width.
std::vector<PavedBorderRun> BuildPavedBorder(std::span<const PavedBoundaryRun> runs,
                                             const PavedBorderSettings& settings, const GroundSampler& groundAt,
                                             std::span<const PavedVerge> armVerges);

} // namespace GameEngine::SplineGeometry
