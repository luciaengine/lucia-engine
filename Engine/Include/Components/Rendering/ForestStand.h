#pragma once

#include "Components/AssetRef.h"
#include "Types/Types.h"

#include <type_traits>

namespace GameEngine {
namespace Components {

/// What the trees beyond the near meshes draw as, out to Detail Distance.
enum class ForestFarRepresentation : uint8
{
    /// The impostor card that the engine bakes from the tree model.
    Cards = 0,
    /// Animated splats from the stand's splat bake (ForestPlacementCook --splats). The splats fade into the
    /// overview over the last 64 meters past Detail Distance.
    Splats = 1,
};

/// A cooked forest stand on the entity whose MeshRenderer holds its overview mesh, with render layers
/// None. It draws the overview and, with a tree model, the trees near the camera, at the archive's
/// world positions; the entity's transform has no effect.
struct ForestStand
{
    /// The highest Near Mesh Budget that a device tier draws.
    static constexpr uint32 kMaxNearMeshBudget = 256;

    /// The packed placement archive (.geforest) that the overview mesh was cooked from.
    ForestPlacementArchiveRef Archive;
    /// The tree model with its impostor setting on. Empty draws the overview only.
    ModelRef TreeModel;
    /// Meters from the camera within which visible pages can draw trees, up to the detail slot budget.
    /// 0 draws the overview only.
    float32 DetailDistance = 400.0f;
    /// The most placements near the camera that draw the tree mesh instead of its impostor card.
    /// 0 draws cards only. The device tier caps it at kMaxNearMeshBudget, or 128 on the compatibility tier.
    uint32 NearMeshBudget = 0;
    /// The splat bake (.geforestsplats) of the tree model, which Splats draws. ForestPlacementCook --splats
    /// writes it.
    ForestSplatsRef Splats;
    /// What the trees beyond the near meshes draw as.
    ForestFarRepresentation FarRepresentation = ForestFarRepresentation::Cards;
};

static_assert(std::is_trivially_copyable_v<ForestStand>, "ForestStand must be trivially copyable for ECS");
static_assert(std::is_standard_layout_v<ForestStand>, "ForestStand must be standard layout for ECS");

} // namespace Components
} // namespace GameEngine
