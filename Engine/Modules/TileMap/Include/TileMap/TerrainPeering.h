#pragma once

#include "AssetCore/Types.h"

#include <array>

namespace GameEngine
{
namespace TileMap
{

/**
 * @brief Neighbor slots for terrain peering, clockwise from top-left.
 *
 * Indexes into TerrainPeering::Neighbors. Sides participate in side-matching
 * terrain sets, corners in corner-matching sets, both in mixed sets.
 */
enum class PeeringSlot : uint8
{
    TopLeft = 0,
    Top = 1,
    TopRight = 2,
    Right = 3,
    BottomRight = 4,
    Bottom = 5,
    BottomLeft = 6,
    Left = 7
};

inline constexpr uint32 kPeeringSlotCount = 8;

/** Cell-space offset of one peering slot. Cell Y grows upward, so Top is +Y. */
struct PeeringSlotOffset
{
    int32 X = 0;
    int32 Y = 0;
};

inline constexpr std::array<PeeringSlotOffset, kPeeringSlotCount> kPeeringSlotOffsets{{
    {-1, 1}, {0, 1}, {1, 1}, {1, 0}, {1, -1}, {0, -1}, {-1, -1}, {-1, 0}}};

/** Sides are the odd slots, corners the even ones, given the clockwise order. */
constexpr bool IsSidePeeringSlot(uint32 slot) { return (slot & 1u) != 0u; }

/**
 * @brief Per-tile terrain annotation: which terrain this tile's center and
 * each of its 8 neighbor edges/corners belong to.
 *
 * Terrain indices refer into the owning tileset's terrain-set definition;
 * -1 means "no terrain" (matches empty/other-terrain neighbors). A tile with
 * TerrainSet == -1 does not participate in terrain matching at all.
 */
struct TerrainPeering
{
    int16 TerrainSet = -1;
    int16 Center = -1;
    std::array<int16, kPeeringSlotCount> Neighbors{-1, -1, -1, -1, -1, -1, -1, -1};

    bool ParticipatesInTerrain() const { return TerrainSet >= 0; }

    bool operator==(const TerrainPeering&) const = default;
};

/**
 * @brief Which neighbor slots a terrain set matches on (Godot's three modes).
 */
enum class TerrainMatchMode : uint8
{
    CornersAndSides = 0,
    Corners = 1,
    Sides = 2
};

} // namespace TileMap
} // namespace GameEngine
