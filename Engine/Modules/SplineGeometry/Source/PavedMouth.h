#pragma once

#include "PavedTaggedRing.h"

#include "Types/Types.h"

#include <span>
#include <vector>

namespace GameEngine::SplineGeometry
{

class PavedArmPath;

// The round mouth of one arm where the arm leaves an outline.
struct PavedMouth
{
    // One ring per round side: the corner between the arm's side and the outline, filled up to an arc
    // tangent to both. Every edge is tagged Mouth.
    std::vector<PavedTaggedRing> Rings;
    // Meters along the arm from the node to the farthest point where an arc meets the arm's side.
    float32 Throat = 0.0f;
    // Both sides are round, so no sharp corner is left for the stop's advance to clear.
    bool BothSides = false;
};

// The mouth of each arm in `paths` that leaves an outline with a `radii` entry above 0: an arc of that radius at
// each side's corner. The arc gets smaller to fit: half the way to the next outline corner, and on the arm before
// its end and within the larger of three arm widths and the radius. Under kMinPavedMouthRadiusMeters, no arc.
std::vector<PavedMouth> BuildPavedMouths(std::span<const PavedArmPath> paths, std::span<const uint8> armValid,
                                         std::span<const PavedTaggedRing> outlines, std::span<const float32> radii);

// The smallest arc a mouth gets; a corner whose arc must be smaller stays sharp.
inline constexpr float32 kMinPavedMouthRadiusMeters = 0.05f;

} // namespace GameEngine::SplineGeometry
