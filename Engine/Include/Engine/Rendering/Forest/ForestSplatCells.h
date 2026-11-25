#pragma once

#include "Mathematics/Vector3.h"
#include "Types/Types.h"

#include <array>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace GameEngine::Engine::Renderer
{

/// One cell of a tree's splat bake: a disk that stands for the leaves or bark inside one grid cell.
/// The three words are the GPU layout that the forest splat shader reads.
struct ForestSplatCell
{
    /// Byte 0 to 2: grid X, Y, Z. Byte 3: coverage, the fraction of the cell's face that the disk covers.
    uint32 PositionCoverage;
    /// Byte 0 to 2: linear RGB albedo. Byte 3: octahedral normal X.
    uint32 ColorNormalX;
    /// Byte 0: octahedral normal Y. Byte 1: wind bend weight. Byte 2: leaf fraction. Byte 3: zero.
    uint32 NormalYBendLeaf;
};
static_assert(sizeof(ForestSplatCell) == 12);

/// The splat bake of one tree model (.geforestsplats), in the model space of the imported model at unit scale.
/// The cell at grid (x, y, z) has its center at Origin + CellSize * (x, y, z).
struct ForestSplatCells
{
    /// The most cells in one bake. Each cell draws four vertices with 16-bit indices.
    static constexpr uint32 kMaxCells = 16384;
    /// The size of the file header.
    static constexpr uint64 kHeaderBytes = 32;
    /// The size of the largest valid file.
    static constexpr uint64 kMaxFileBytes = kHeaderBytes + uint64{kMaxCells} * sizeof(ForestSplatCell);

    Mathematics::Vector3 Origin;
    float CellSize = 0.0f;
    std::vector<ForestSplatCell> Cells;
};

/// Reads a .geforestsplats file. Returns nothing and sets error when the bytes are not a valid bake.
std::optional<ForestSplatCells> ParseForestSplatCells(std::span<const uint8> bytes, std::string& error);

/// Writes a bake in the .geforestsplats format. The cells must be valid for ParseForestSplatCells.
std::vector<uint8> EncodeForestSplatCells(const ForestSplatCells& cells);

/// Groups fine splats into twice-wide cells. Leaves and bark keep separate disks.
std::optional<ForestSplatCells> CoarsenForestSplatCells(const ForestSplatCells& fine);

} // namespace GameEngine::Engine::Renderer
