#pragma once

#include "Mathematics/Interpolation.h"
#include "Mathematics/Vector3.h"
#include "Mathematics/WorldPosition.h"
#include "Types/Types.h"

#include <filesystem>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

namespace GameEngine::PageStreaming
{

/// One tree in a species' placement store. X, Z and Height are centimeters from the page origin.
/// Yaw is a full turn in 256 steps and the uniform scale spans [0.75, 1.25] in 255 steps.
struct PackedPlacement
{
    uint16 X;
    uint16 Z;
    uint16 Height;
    uint8 Yaw;
    uint8 Scale;

    static constexpr float kMinScale = 0.75f;
    static constexpr float kMaxScale = 1.25f;
    static constexpr float kScaleSteps = 255.0f;
    static constexpr double kYawSteps = 256.0;
    static constexpr double kMetersPerStep = 0.01;

    constexpr double LocalX() const { return X * kMetersPerStep; }
    constexpr double LocalY() const { return Height * kMetersPerStep; }
    constexpr double LocalZ() const { return Z * kMetersPerStep; }
    constexpr double YawRadians() const { return Yaw * (2.0 * std::numbers::pi / kYawSteps); }
    constexpr float UniformScale() const { return Math::Lerp(kMinScale, kMaxScale, Scale / kScaleSteps); }
};
static_assert(sizeof(PackedPlacement) == 8);

/// One page of a store and the matching quad of the overview mesh.
/// LocalMin and LocalMax bound every tree of the page, in meters from Origin.
struct PlacementPage
{
    Mathematics::WorldPosition Origin;
    Mathematics::Vector3 LocalMin;
    Mathematics::Vector3 LocalMax;
    uint64 ContentHash;
    uint32 First;
    uint32 Count;
};
static_assert(sizeof(PlacementPage) == 64 && std::is_trivially_copyable_v<PlacementPage>);

/// The bytes of one page's placements in the store file.
struct PlacementByteRange
{
    uint64 Offset;
    uint64 Bytes;
};

/// Writes a store with the pages in page order and computes the page hashes from the placements.
/// First and Count must cover the placements contiguously. boundRadius is the distance in meters from a
/// root to the farthest point of the tree at unit scale, which the page bounds include.
/// The target is replaced only after a complete write. Returns an empty string on success.
std::string WritePackedPlacementStore(const std::filesystem::path& target, float boundRadius,
                                      std::span<const PlacementPage> pages, std::span<const PackedPlacement> placements);

/// The validated in-memory directory of one immutable .geforest store.
/// Open must finish before the store is shared. After that, every const member is safe to call from
/// any thread. The caller reads page ranges itself and verifies each page before it uses it.
class PackedPlacementStore
{
public:
    static constexpr uint32 kPlacementsPerPage = 144;
    static constexpr uint32 kMaxPages = 1u << 20;

    /// Returns an empty string on success. A failed open clears the directory.
    std::string Open(const std::filesystem::path& file);

    /// The file of the last successful Open.
    const std::filesystem::path& File() const { return m_File; }
    /// The page directory in page order.
    std::span<const PlacementPage> Pages() const { return m_Pages; }
    /// The bound radius the pages were cooked with, in meters at unit scale.
    float BoundRadius() const { return m_BoundRadius; }

    /// The byte range of a page in File(), or nothing for a page outside the directory.
    std::optional<PlacementByteRange> ReadRange(uint32 page) const;
    /// True when bytes are exactly the page's range and match its hash.
    bool VerifyPage(uint32 page, std::span<const uint8> bytes) const;

private:
    std::filesystem::path m_File;
    std::vector<PlacementPage> m_Pages;
    uint64 m_DataOffset = 0;
    float m_BoundRadius = 0.0f;
};

} // namespace GameEngine::PageStreaming
