#pragma once

#include "Components/Spline/SplineExtrude.h"
#include "Components/Terrain/TerrainModifierEffects.h"
#include "Components/Terrain/TerrainModifierVolume.h"
#include "ECS/Entity.h"

#include <optional>

namespace GameEngine::ECS { class World; }

namespace GameEngine::SplineRecipes
{

// The terrain pieces of one entity that the ground authority writes, each present or absent.
struct SplineGroundPieces
{
    std::optional<Components::TerrainFlattenEffect> Flatten;
    std::optional<Components::TerrainGroundClaimEffect> Claim;
    std::optional<Components::TerrainModifierVolume> Volume;
    std::optional<Components::TerrainGrassEffect> Grass;
    std::optional<Components::TerrainPaintLayerEffect> Paint;
    std::optional<Components::SplineGroundProvisioned> Marker;

    // The pieces on `entity` in `world` now.
    static SplineGroundPieces Read(const ECS::World& world, ECS::EntityHandle entity);
};

// What one sweep of the ground authority did to one entity: its pieces before and after. Apply and
// Revert write only the pieces that differ between the two, so a piece the sweep left alone (a
// hand-authored one) is never in the edit.
struct SplineGroundEdit
{
    ECS::EntityHandle Entity;
    SplineGroundPieces Before;
    SplineGroundPieces After;

    // True when no piece differs.
    bool Empty() const;
    // True when a piece present before is absent after.
    bool Removes() const;
    // Writes the pieces that differ as After has them. Does nothing on an invalid entity.
    void Apply(ECS::World& world) const;
    // Writes the pieces that differ as Before has them. Does nothing on an invalid entity.
    void Revert(ECS::World& world) const;
};

} // namespace GameEngine::SplineRecipes
