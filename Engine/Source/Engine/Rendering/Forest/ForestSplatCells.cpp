#include "Engine/Rendering/Forest/ForestSplatCells.h"

#include "Mathematics/Octahedral.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <utility>

namespace GameEngine::Engine::Renderer
{
namespace
{
// Little-endian file header. The cells follow it.
struct ForestSplatHeader
{
    std::array<char, 4> Magic;
    uint32 Version;
    uint32 CellCount;
    float OriginX;
    float OriginY;
    float OriginZ;
    float CellSize;
    uint32 Reserved;
};
static_assert(sizeof(ForestSplatHeader) == ForestSplatCells::kHeaderBytes);

constexpr std::array<char, 4> kMagic{'G', 'E', 'F', 'S'};
constexpr uint32 kVersion = 1;

constexpr uint32 Byte(uint32 word, uint32 index)
{
    return (word >> (index * 8u)) & 255u;
}

constexpr uint32 CoarseCellKey(const ForestSplatCell& cell)
{
    constexpr uint32 kPositionMask = 0x007f7f7fu;
    const uint32 position = (cell.PositionCoverage >> 1u) & kPositionMask;
    return position | (Byte(cell.NormalYBendLeaf, 2u) >= 128u ? 0x01000000u : 0u);
}

ForestSplatCell MergeDiskGroup(uint32 key,
                             std::span<const std::pair<uint32, const ForestSplatCell*>> group)
{
    Mathematics::Vector3 color{};
    Mathematics::Vector3 normal{};
    float wind = 0.0f;
    float leaves = 0.0f;
    float area = 0.0f;
    const auto* strongest = group.front().second;
    for (const auto& entry : group)
    {
        const auto* cell = entry.second;
        const float weight = static_cast<float>(Byte(cell->PositionCoverage, 3u));
        color = color + Mathematics::Vector3{static_cast<float>(Byte(cell->ColorNormalX, 0u)),
                                             static_cast<float>(Byte(cell->ColorNormalX, 1u)),
                                             static_cast<float>(Byte(cell->ColorNormalX, 2u))} * weight;
        normal = normal + Mathematics::OctahedralDecode({Byte(cell->ColorNormalX, 3u) / 255.0f,
                                                          Byte(cell->NormalYBendLeaf, 0u) / 255.0f}) * weight;
        wind += static_cast<float>(Byte(cell->NormalYBendLeaf, 1u)) * weight;
        leaves += static_cast<float>(Byte(cell->NormalYBendLeaf, 2u)) * weight;
        area += weight;
        if (Byte(cell->PositionCoverage, 3u) > Byte(strongest->PositionCoverage, 3u))
            strongest = cell;
    }
    if (normal.LengthSquared() < 1.0e-6f)
        normal = Mathematics::OctahedralDecode({Byte(strongest->ColorNormalX, 3u) / 255.0f,
                                                 Byte(strongest->NormalYBendLeaf, 0u) / 255.0f});
    const auto oct = Mathematics::OctahedralEncode(normal);
    const auto byte = [](float value) { return static_cast<uint32>(std::clamp(std::lround(value), 0l, 255l)); };
    const float inverseArea = 1.0f / area;
    // A disk of twice the width has four times the area at the same coverage.
    const uint32 coverage = std::max(1u, byte(area * 0.25f));
    return {(key & 0x00ffffffu) | (coverage << 24u),
            byte(color.x * inverseArea) | (byte(color.y * inverseArea) << 8u) |
                (byte(color.z * inverseArea) << 16u) | (byte(oct.x * 255.0f) << 24u),
            byte(oct.y * 255.0f) | (byte(wind * inverseArea) << 8u) | (byte(leaves * inverseArea) << 16u)};
}
} // namespace

std::optional<ForestSplatCells> ParseForestSplatCells(std::span<const uint8> bytes, std::string& error)
{
    ForestSplatHeader header{};
    if (bytes.size() < sizeof(header))
    {
        error = "the file is shorter than its header";
        return std::nullopt;
    }
    std::memcpy(&header, bytes.data(), sizeof(header));
    if (header.Magic != kMagic || header.Version != kVersion || header.Reserved != 0)
    {
        error = "the file is not a version 1 .geforestsplats bake";
        return std::nullopt;
    }
    if (header.CellCount == 0 || header.CellCount > ForestSplatCells::kMaxCells)
    {
        error = std::format("the bake has {} cells; a bake has 1 to {}", header.CellCount, ForestSplatCells::kMaxCells);
        return std::nullopt;
    }
    if (bytes.size() != sizeof(header) + size_t{header.CellCount} * sizeof(ForestSplatCell))
    {
        error = "the file size does not match its cell count";
        return std::nullopt;
    }
    if (!std::isfinite(header.OriginX) || !std::isfinite(header.OriginY) || !std::isfinite(header.OriginZ) ||
        !std::isfinite(header.CellSize) || header.CellSize <= 0.0f)
    {
        error = "the bake has a grid origin or cell size that is not a finite number of meters";
        return std::nullopt;
    }

    ForestSplatCells cells{.Origin = {header.OriginX, header.OriginY, header.OriginZ}, .CellSize = header.CellSize};
    cells.Cells.resize(header.CellCount);
    std::memcpy(cells.Cells.data(), bytes.data() + sizeof(header), cells.Cells.size() * sizeof(ForestSplatCell));
    return cells;
}

std::vector<uint8> EncodeForestSplatCells(const ForestSplatCells& cells)
{
    const ForestSplatHeader header{kMagic, kVersion, static_cast<uint32>(cells.Cells.size()), cells.Origin.x,
                                   cells.Origin.y, cells.Origin.z, cells.CellSize, 0};
    std::vector<uint8> bytes(sizeof(header) + cells.Cells.size() * sizeof(ForestSplatCell));
    std::memcpy(bytes.data(), &header, sizeof(header));
    std::memcpy(bytes.data() + sizeof(header), cells.Cells.data(), cells.Cells.size() * sizeof(ForestSplatCell));
    return bytes;
}

std::optional<ForestSplatCells> CoarsenForestSplatCells(const ForestSplatCells& fine)
{
    const float cellSize = 2.0f * fine.CellSize;
    if (!std::isfinite(cellSize) || cellSize <= 0.0f || fine.Cells.empty())
        return std::nullopt;

    std::vector<std::pair<uint32, const ForestSplatCell*>> sorted;
    sorted.reserve(fine.Cells.size());
    for (const auto& cell : fine.Cells)
        if (Byte(cell.PositionCoverage, 3u) != 0u)
            sorted.emplace_back(CoarseCellKey(cell), &cell);
    const auto keyOf = [](const auto& entry) { return entry.first; };
    std::ranges::stable_sort(sorted, {}, keyOf);
    const float halfCell = 0.5f * fine.CellSize;
    ForestSplatCells coarse{.Origin = fine.Origin + Mathematics::Vector3{halfCell, halfCell, halfCell},
                            .CellSize = cellSize};
    for (size_t first = 0; first < sorted.size();)
    {
        const uint32 key = sorted[first].first;
        const auto remaining = std::span(sorted).subspan(first);
        const auto end = std::ranges::upper_bound(remaining, key, {}, keyOf);
        const size_t count = static_cast<size_t>(end - remaining.begin());
        coarse.Cells.push_back(MergeDiskGroup(key, remaining.first(count)));
        first += count;
    }
    if (coarse.Cells.empty() || coarse.Cells.size() == fine.Cells.size())
        return std::nullopt;
    return coarse;
}

} // namespace GameEngine::Engine::Renderer
