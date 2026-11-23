#include "Engine/Rendering/Forest/ForestSplatCells.h"

#include <cmath>
#include <cstring>
#include <format>

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

} // namespace GameEngine::Engine::Renderer
