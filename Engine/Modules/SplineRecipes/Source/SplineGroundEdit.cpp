#include "SplineRecipes/SplineGroundEdit.h"

#include "ECS/ECSTemplates.h"
#include "ECS/World.h"

#include <cstring>

namespace GameEngine::SplineRecipes
{
namespace
{

template <typename Piece>
std::optional<Piece> ReadPiece(const ECS::World& world, ECS::EntityHandle entity)
{
    const Piece* piece = world.GetComponent<Piece>(entity);
    return piece ? std::optional<Piece>(*piece) : std::nullopt;
}

// Two copies of one trivially copyable piece read the same, byte for byte.
template <typename Piece>
bool SamePiece(const std::optional<Piece>& a, const std::optional<Piece>& b)
{
    if (a.has_value() != b.has_value())
        return false;
    return !a || std::memcmp(&*a, &*b, sizeof(Piece)) == 0;
}

template <typename Piece>
bool RemovesPiece(const std::optional<Piece>& before, const std::optional<Piece>& after)
{
    return before.has_value() && !after.has_value();
}

// Writes `to` on `entity` when it differs from `from`; an absent `to` removes the piece.
template <typename Piece>
void WritePiece(ECS::World& world, ECS::EntityHandle entity, const std::optional<Piece>& from,
                const std::optional<Piece>& to)
{
    if (SamePiece(from, to))
        return;
    if (to)
        world.AddComponentImmediate(entity, *to);
    else if (world.GetComponent<Piece>(entity))
        world.RemoveComponentImmediate<Piece>(entity);
}

void WritePieces(ECS::World& world, ECS::EntityHandle entity, const SplineGroundPieces& from,
                 const SplineGroundPieces& to)
{
    if (!world.IsValid(entity))
        return;
    WritePiece(world, entity, from.Flatten, to.Flatten);
    WritePiece(world, entity, from.Claim, to.Claim);
    WritePiece(world, entity, from.Volume, to.Volume);
    WritePiece(world, entity, from.Grass, to.Grass);
    WritePiece(world, entity, from.Paint, to.Paint);
    WritePiece(world, entity, from.Marker, to.Marker);
}

} // namespace

SplineGroundPieces SplineGroundPieces::Read(const ECS::World& world, ECS::EntityHandle entity)
{
    return {.Flatten = ReadPiece<Components::TerrainFlattenEffect>(world, entity),
            .Claim = ReadPiece<Components::TerrainGroundClaimEffect>(world, entity),
            .Volume = ReadPiece<Components::TerrainModifierVolume>(world, entity),
            .Grass = ReadPiece<Components::TerrainGrassEffect>(world, entity),
            .Paint = ReadPiece<Components::TerrainPaintLayerEffect>(world, entity),
            .Marker = ReadPiece<Components::SplineGroundProvisioned>(world, entity)};
}

bool SplineGroundEdit::Empty() const
{
    return SamePiece(Before.Flatten, After.Flatten) && SamePiece(Before.Claim, After.Claim)
           && SamePiece(Before.Volume, After.Volume) && SamePiece(Before.Grass, After.Grass)
           && SamePiece(Before.Paint, After.Paint) && SamePiece(Before.Marker, After.Marker);
}

bool SplineGroundEdit::Removes() const
{
    return RemovesPiece(Before.Flatten, After.Flatten) || RemovesPiece(Before.Claim, After.Claim)
           || RemovesPiece(Before.Volume, After.Volume) || RemovesPiece(Before.Grass, After.Grass)
           || RemovesPiece(Before.Paint, After.Paint);
}

void SplineGroundEdit::Apply(ECS::World& world) const
{
    WritePieces(world, Entity, Before, After);
}

void SplineGroundEdit::Revert(ECS::World& world) const
{
    WritePieces(world, Entity, After, Before);
}

} // namespace GameEngine::SplineRecipes
