#pragma once

#include "Components/Spline/SplineComponent.h"
#include "Components/Terrain/TerrainModifierEffects.h"
#include "ECS/ChangeFilter.h"
#include "ECS/Entity.h"
#include "SplineRecipes/Paving/PavingApproach.h"
#include "Types/Types.h"

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace GameEngine::ECS { class World; }

namespace GameEngine::SplineRecipes
{

// The approaches of a paved area on Owns: a runtime-only Spline Path piece under the paving for each,
// with copies of the paving's grass, paint and volume settings. Under a road that does not grade, the
// piece also copies the flatten and the claim. A road on Owns keeps its own ground.
class PavingApproachGround
{
public:
    // Lays, rewrites or removes the pieces for `approaches` (SplinePavingController::Approaches),
    // whose contents change only when `approachesVersion` does. Reads the world only when that
    // version, a paving, a road's extrude or a terrain effect changed since the last call.
    void Update(ECS::World& world, std::span<const PavingApproach> approaches, uint64 approachesVersion);

    // Destroys every piece.
    void Shutdown(ECS::World& world);

    // The calls to Update that read the world.
    uint32 ReadsForTests() const { return m_Reads; }

private:
    // What a piece copies from the paving that owns it.
    struct Copied
    {
        std::optional<Components::TerrainGrassEffect> Grass;
        std::optional<Components::TerrainPaintLayerEffect> Paint;
        std::optional<Components::TerrainFlattenEffect> Flatten;
        std::optional<Components::TerrainGroundClaimEffect> Claim;
        float32 Falloff = 0.0f;
        float32 Weight = 0.0f;
        float32 Priority = 0.0f;
    };

    // One approach's piece: what it is laid from, and its entity and spline once laid.
    struct Piece
    {
        PavingApproach Approach;
        Copied Effects;
        ECS::EntityHandle Entity{};
        Components::SplineComponent Spline{};
    };

    bool InputsChanged(ECS::World& world, uint64 approachesVersion) const;
    // The pieces `approaches` ask for in `world` now, not laid yet.
    std::vector<Piece> Wanted(ECS::World& world, std::span<const PavingApproach> approaches) const;
    // True when `wanted` are the pieces laid now.
    bool Laid(const ECS::World& world, std::span<const Piece> wanted) const;
    void Lay(ECS::World& world, Piece& piece, uint32 index) const;

    std::vector<Piece> m_Pieces;
    ECS::ChangeGate m_Gate;
    std::size_t m_StructuralVersion = 0;
    uint64 m_ApproachesVersion = 0;
    bool m_Primed = false;
    uint32 m_Reads = 0;
};

} // namespace GameEngine::SplineRecipes
