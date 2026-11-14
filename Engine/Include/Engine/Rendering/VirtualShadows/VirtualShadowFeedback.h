#pragma once

#include "Engine/Rendering/VirtualShadows/VirtualShadowPageCache.h"
#include "Mathematics/Vector3.h"
#include "Mathematics/Vector3Int.h"
#include "Mathematics/WorldPosition.h"

#include <array>

namespace GameEngine::Engine::Renderer
{

inline constexpr uint32_t kVirtualShadowMaxSpotProjections = 8;
inline constexpr uint32_t kVirtualShadowPhysicalTiles = 256;
inline constexpr uint8_t kVirtualShadowCasterCullingIndex = 0xFBu;

bool VirtualShadowFeedbackEnabled();

struct ShadowCasterChangeSphere;
struct VirtualShadowDirectionalGrid
{
    std::array<Mathematics::Vector3, 3> LightAxes;
    double PageWorldSize;
    double DepthStep;
    int64_t DepthBand;

    bool operator==(const VirtualShadowDirectionalGrid& other) const
    {
        for (size_t axis = 0; axis < LightAxes.size(); ++axis)
            for (size_t component = 0; component < 3; ++component)
                if (LightAxes[axis][component] != other.LightAxes[axis][component])
                    return false;
        return PageWorldSize == other.PageWorldSize && DepthStep == other.DepthStep &&
               DepthBand == other.DepthBand;
    }
};

struct VirtualShadowFeedbackProjection
{
    // BasePage.X/Y are the world-page origin for a directional window.
    // Local lights start at (0, 0) and retain their independent mip chains.
    VirtualShadowPage BasePage;
    Mathematics::Matrix4x4 WorldToLight;
    uint32_t FirstPage = 0;
    uint32_t PagesPerAxis = 0;
    uint32_t LastMip = 0;
    std::optional<VirtualShadowDirectionalGrid> DirectionalGrid{};
    std::optional<Mathematics::WorldPosition> LocalPosition{};
};

struct VirtualShadowDirectionalWindow
{
    VirtualShadowPage Origin;
    VirtualShadowDirectionalGrid Grid;
    Mathematics::Matrix4x4 WorldToLight;
    Mathematics::Matrix4x4 RelativeToLight;
};

std::optional<VirtualShadowDirectionalWindow> BuildVirtualShadowDirectionalWindow(
    uint64_t lightId, uint32_t level, uint32_t levelCount, uint32_t pagesPerAxis, float shadowDistance,
    const Mathematics::Matrix4x4& lightRotation, const Mathematics::WorldPosition& worldEye,
    const Mathematics::Vector3Int& renderOriginSector);

// The same world address yields the same raster projection as its window moves.
Mathematics::Matrix4x4 VirtualShadowDirectionalPageMatrix(
    const VirtualShadowDirectionalGrid& grid, const VirtualShadowPage& page,
    const Mathematics::Vector3Int& renderOriginSector);

inline constexpr double kVirtualShadowDirectionalDepthBands = 6.0;

struct VirtualShadowFeedbackLayout
{
    uint64_t World = 0;
    uint32_t RenderLayerMask = 0;
    uint32_t WordCount = 0;
    std::vector<VirtualShadowFeedbackProjection> Projections;
};

bool SameVirtualShadowProjection(const VirtualShadowFeedbackProjection& a,
                                 const VirtualShadowFeedbackProjection& b);

// The address of a page in this dispatch's bitset (also the page-table index).
std::optional<uint32_t> FindVirtualShadowFeedbackBit(
    const VirtualShadowFeedbackProjection& projection, const VirtualShadowPage& page);

bool VirtualShadowPageTouchedByCasters(const VirtualShadowFeedbackProjection& projection,
                                       const VirtualShadowPage& page,
                                       std::span<const ShadowCasterChangeSphere> changes);

// Reconcile one completed dispatch with the current light geometry. Admit
// local fallback mips first, then give each matching projection a turn.
std::vector<VirtualShadowPage> DecodeVirtualShadowFeedback(
    std::span<const uint32_t> words, const VirtualShadowFeedbackLayout& dispatched,
    const VirtualShadowFeedbackLayout& current);

} // namespace GameEngine::Engine::Renderer
