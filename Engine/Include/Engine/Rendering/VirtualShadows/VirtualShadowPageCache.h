#pragma once

#include "Mathematics/Matrix4x4.h"
#include "Types/FlatMap.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <vector>

namespace GameEngine::Engine::Renderer
{

inline constexpr uint32_t kVirtualShadowPageTexels = 128;
inline constexpr uint32_t kVirtualLocalShadowPagesPerAxis = 128;
inline constexpr uint32_t kVirtualShadowClipLevels = 16;
inline constexpr uint32_t kUnmappedShadowPage = std::numeric_limits<uint32_t>::max();

enum class VirtualShadowLight : uint8_t
{
    Directional,
    Spot,
    Point,
};

struct VirtualShadowPage
{
    uint64_t LightId = 0;
    VirtualShadowLight Family = VirtualShadowLight::Directional;
    uint8_t Projection = 0; // Clip level for a sun, cube face for a point light.
    uint8_t Mip = 0;
    int64_t X = 0; // World-fixed signed coordinates for directional clipmaps.
    int64_t Y = 0;

    auto operator<=>(const VirtualShadowPage&) const = default;
};

// Crop an existing light projection to one page for fixed-function depth
// raster. Page coordinates start at the top left and clip Y points up.
std::optional<Mathematics::Matrix4x4> CropVirtualShadowProjection(
    const Mathematics::Matrix4x4& lightProjection, uint32_t pagesPerAxis, uint32_t x, uint32_t y);

struct VirtualShadowPageWork
{
    VirtualShadowPage Page;
    uint32_t Tile = kUnmappedShadowPage;
    uint64_t Revision = 0;
    bool NeedsRender = false;
};

/// Most page depth passes that one view records in one frame.
inline constexpr uint32_t kVirtualShadowPageRenderBudget = 24;
/// Most deformed pages that one view refreshes in one frame, after the missing pages.
inline constexpr uint32_t kVirtualShadowDeformedRefreshBudget = 16;
/// Frames a deformed page of the finest level may wait. Each coarser level doubles it.
inline constexpr uint64_t kVirtualShadowDeformedMaxAgeFrames = 2;

// CPU reference for a per-view page table backed by a bounded depth atlas.
class VirtualShadowPageCache
{
public:
    explicit VirtualShadowPageCache(uint32_t tileCount);

    // The caller supplies visible pages in priority order, including coarse
    // fallback pages it will publish. Distinct, valid requests each get a row.
    // The first tileCount rows are eligible for residency. An in-flight lower
    // priority tile may delay a new mapping until completedFrame advances.
    // Deformed pages refresh in the render budget that missing pages leave: pages over
    // their age limit first, then finer levels, then older pages.
    std::vector<VirtualShadowPageWork> Plan(
        std::span<const VirtualShadowPage> requests, uint64_t frame, uint64_t completedFrame);

    // Refresh a page in place after ordering the atlas writes against reads
    // in the render graph. Publish only after the actual depth pass submits.
    bool MarkRendered(const VirtualShadowPageWork& work);
    /// Whether this tile still names the same page and content revision.
    bool Contains(const VirtualShadowPageWork& work) const;
    void MarkDirty(const VirtualShadowPage& page);
    void InvalidateLight(VirtualShadowLight family, uint64_t lightId);
    void InvalidateProjection(const VirtualShadowPage& origin);
    void InvalidateAll();
    /// Vertex deformation moved casters in the rendered pages. The pages stay
    /// published and refresh over the next frames, as Plan describes.
    void MarkDeformed();

    template <class Predicate>
    void InvalidatePages(Predicate&& matches)
    {
        for (auto& tile : m_Tiles)
            if (tile.Page && matches(*tile.Page))
                Invalidate(tile);
    }

private:
    struct PageHash
    {
        std::size_t operator()(const VirtualShadowPage& page) const noexcept;
    };

    struct Tile
    {
        std::optional<VirtualShadowPage> Page;
        uint64_t LastUse = 0;
        uint64_t Revision = 0;
        uint64_t RenderFrame = 0;
        bool Rendered = false;
        bool Deformed = false;
    };

    void Invalidate(Tile& tile);
    void RefreshDeformed(std::span<VirtualShadowPageWork> rows, uint64_t frame);

    std::vector<Tile> m_Tiles;
    FlatMap<VirtualShadowPage, uint32_t> m_Mappings;
    uint64_t m_NextRevision = 1;
};

} // namespace GameEngine::Engine::Renderer
