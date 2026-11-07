#pragma once

#include "ECS/Entity.h"
#include "SplineECS/SplineNetwork.h"
#include "SplineRecipes/Paving/PavingPlan.h"
#include "Types/Types.h"

#include <string>
#include <vector>

namespace GameEngine::SplineRecipes
{

// What a paving does at one junction on its spline.
enum class PavingJunctionState : uint8
{
    // It paves the junction.
    Paved = 0,
    // Another paving's region holds the junction (PavingJunctionStatus::PavedBy).
    PavedByOther,
    // The two heights differ by more than a road joins within: nothing is paved there.
    HeightsDiffer,
    // The road is a member with Ignore: nothing is paved there.
    Ignored,
    // The road is a member with Exclude: the paving is cut out along its strip.
    CutOut,
    // The road is too narrow where it meets: nothing is paved there.
    TooNarrow,
};

// One junction a paving reads, as its last rebuild found it.
struct PavingJunctionStatus
{
    // The junction's id in the spline network (SplineECS::SplineNode::Id).
    uint32 NodeId = 0;
    SplineECS::SplineNodeKind Kind = SplineECS::SplineNodeKind::Crossing;
    // The other spline at the junction.
    ECS::EntityHandle Road{};
    // Meters along the other spline from its start to the junction.
    float32 RoadMeters = 0.0f;
    PavingJunctionState State = PavingJunctionState::Paved;
    // The paving that builds the junction, when State is PavedByOther.
    ECS::EntityHandle PavedBy{};
    // When State is HeightsDiffer: the meters the road lies above this spline; negative below.
    float32 RoadAboveMeters = 0.0f;

    bool operator==(const PavingJunctionStatus&) const = default;
};

// A paving as its last rebuild left it: what it does, its junctions, and the notices that
// state each fix.
struct PavingStatus
{
    ECS::EntityHandle Entity{};
    PavingRole Role = PavingRole::NothingToPave;
    // The paving that builds every region it reaches, when Role is PavedBy.
    ECS::EntityHandle PavedBy{};
    std::vector<PavingJunctionStatus> Junctions;
    std::vector<std::string> Notices;

    bool operator==(const PavingStatus&) const = default;
};

} // namespace GameEngine::SplineRecipes
