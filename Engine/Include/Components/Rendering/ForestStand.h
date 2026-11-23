#pragma once

#include "Components/AssetRef.h"
#include "Types/Types.h"

#include <type_traits>

namespace GameEngine {
namespace Components {

/// A cooked forest stand on its overview entity: the entity whose MeshRenderer holds the cooked
/// overview mesh with its render layers set to None. The forest stand system draws the overview
/// from the archive's pages and, with a tree model, the trees near the camera. The stand draws at
/// the archive's world positions; the system ignores the entity's transform.
struct ForestStand
{
    /// The packed placement archive (.geforest) that the overview mesh was cooked from.
    ForestPlacementArchiveRef Archive;
    /// The tree model with its impostor setting on. Empty draws the overview only.
    ModelRef TreeModel;
    /// Meters from the camera within which the stand draws trees instead of the overview.
    float32 DetailDistance = 400.0f;
    /// The most placements near the camera that draw the tree mesh instead of its impostor card.
    /// 0 draws cards only. The device tier caps it.
    uint32 NearMeshBudget = 0;
};

static_assert(std::is_trivially_copyable_v<ForestStand>, "ForestStand must be trivially copyable for ECS");
static_assert(std::is_standard_layout_v<ForestStand>, "ForestStand must be standard layout for ECS");

} // namespace Components
} // namespace GameEngine
