#pragma once

#include "ECS/Entity.h"
#include "Mathematics/Vector3.h"
#include "SplineGeometry/AreaGround.h"
#include "Types/Types.h"

#include <span>
#include <vector>

namespace GameEngine::SplineGeometry
{
struct PavedArm;
struct PavedArmStop;
} // namespace GameEngine::SplineGeometry

namespace GameEngine::SplineRecipes
{

// One approach of a paved area: an arm's ribbon from the node on the Area's outline out to the arm's
// approach stop. The paving builder derives it on every rebuild; nothing saves it.
struct PavingApproach
{
    // The paving that owns the arm's region, and the road the arm runs on (its entity id).
    ECS::EntityHandle Owner{};
    uint32 Road = 0;
    // The arm's centerline from the node to the stop, world meters, on the ground under the owner.
    std::vector<Mathematics::Vector3> Points;
    // Per point, the meters from the centerline to the paving's edge on the wider side.
    std::vector<float32> HalfWidths;

    // True when every member is exactly equal.
    bool operator==(const PavingApproach& other) const;
};

// The approach of `arm` up to `stop`, with heights from `ground`, its edge `edgeWidth` meters past the
// ribbon. It starts where the arm last leaves `outline`, so an arm that runs through the outline and
// out along its rim starts at the rim. Empty Points when the stop is at the node or inside `outline`.
PavingApproach BuildPavingApproach(const SplineGeometry::PavedArm& arm, const SplineGeometry::PavedArmStop& stop,
                                   float32 edgeWidth, const SplineGeometry::GroundSampler& ground,
                                   std::span<const Mathematics::Vector2> outline);

} // namespace GameEngine::SplineRecipes
