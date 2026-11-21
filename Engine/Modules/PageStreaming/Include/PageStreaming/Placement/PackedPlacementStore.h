#pragma once

#include "Types/Types.h"

#include <array>
#include <filesystem>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace GameEngine::PageStreaming
{

/// One instance in a species' placement store. X and Z are cell-local centimeters.
/// Yaw is a full turn in 65,536 steps and scale spans [0.75, 1.25].
struct PackedPlacement
{
    uint16 X;
    uint16 Z;
    uint16 Yaw;
    uint16 Scale;

    static constexpr double kMinScale = 0.75;
    static constexpr double kMaxScale = 1.25;

    constexpr double LocalX() const { return X * 0.01; }
    constexpr double LocalZ() const { return Z * 0.01; }
    constexpr double YawRadians() const { return Yaw * (2.0 * std::numbers::pi / 65536.0); }
    constexpr double UniformScale() const { return kMinScale + (kMaxScale - kMinScale) * Scale / 65535.0; }
};
static_assert(sizeof(PackedPlacement) == 8);

/// One detail page and the matching cell of the resident fallback geometry.
struct PlacementPage
{
    std::array<double, 3> Origin;
    std::array<float32, 3> LocalMin;
    std::array<float32, 3> LocalMax;
    uint64 ContentHash;
    uint32 First;
    uint32 Count;
};
static_assert(sizeof(PlacementPage) == 64);

struct PlacementByteRange
{
    uint64 Offset;
    uint64 Bytes;
};

/// Write the pages in stand mesh order and compute hashes from the supplied placements.
/// First and Count must cover placements contiguously. Native publication replaces the target only after a complete write.
std::string WritePackedPlacementStore(const std::filesystem::path& target, std::span<const PlacementPage> pages,
                                      std::span<const PackedPlacement> placements);

/// The validated in-memory directory of one immutable .geforest store.
/// The caller reads individual ranges through the asset reader and validates
/// each page before publishing it to a resident slot. Open must finish before
/// sharing the directory with read and upload workers.
class PackedPlacementStore
{
public:
    static constexpr uint32 kPlacementsPerPage = 144;

    /// Returns an empty string on success. A failed open clears the directory.
    std::string Open(const std::filesystem::path& file);
    const std::filesystem::path& File() const { return m_File; }
    std::span<const PlacementPage> Pages() const { return m_Pages; }
    uint32 PlacementCount() const { return m_PlacementCount; }

    std::optional<PlacementByteRange> ReadRange(uint32 page) const;
    bool VerifyPage(uint32 page, std::span<const uint8> bytes) const;

private:
    std::filesystem::path m_File;
    std::vector<PlacementPage> m_Pages;
    uint64 m_DataOffset = 0;
    uint32 m_PlacementCount = 0;
};

} // namespace GameEngine::PageStreaming
