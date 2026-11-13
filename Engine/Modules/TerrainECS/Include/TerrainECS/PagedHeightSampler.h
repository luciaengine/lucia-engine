#pragma once

#include "PageStreaming/PageAddress.h"
#include "PageStreaming/PageStore/PageStoreFormat.h"
#include "Mathematics/Rect.h"
#include "Types/Types.h"

#include <span>
#include <vector>

namespace GameEngine::PageStreaming
{
class PageCache;
}

// The CPU mirror of the paged height resolve (cbt_page.glsl, slice 1b-ii): the page table, the
// physical cache texture's slot layout, and the sample at a terrain UV and a continuous level. It is
// the oracle the GLSL resolve is held to, as AtlasHeightSampler is for the resident-window atlas.
//
// The resolve at level lambda: L = floor(lambda) clamped to the pyramid, w = PageParentBlend(lambda).
// Level L's sample comes from L's page when resident, faded in from the parent's sample by its
// arrival fade, and from the first resident ancestor alone when not (the fallback walk). The result
// is lerp(level L's sample, level L+1's sample, w), so it reaches the parent exactly at the ring's
// end and is continuous in distance across every ring; the pinned top ends every walk.
namespace GameEngine::TerrainECS
{

class PageTable;

/// The physical cache texture of one field: square-packed slots of kPageStrideSamples texels a side
/// (each page's owned samples and its apron).
struct PageCacheGeometry
{
    uint32 SlotsPerRow = 0;
    uint32 CacheDim = 0; ///< SlotsPerRow * kPageStrideSamples (square)

    /// The cache texel of `slot`'s stride texel (sx, sz).
    uint32 TexelX(uint32 slot, uint32 sx) const { return (slot % SlotsPerRow) * PageStreaming::kPageStrideSamples + sx; }
    uint32 TexelZ(uint32 slot, uint32 sz) const { return (slot / SlotsPerRow) * PageStreaming::kPageStrideSamples + sz; }
};

/// The geometry of a cache of `slotCount` slots.
PageCacheGeometry MakePageCacheGeometry(uint32 slotCount);

/// Where a terrain UV falls at one pyramid level: the page that owns it and its continuous position in
/// that page's stride texels (the apron included), the texel the resolve reads.
struct PageTexel
{
    uint32 PageX = 0;
    uint32 PageZ = 0;
    float32 StrideX = 0.0f;
    float32 StrideZ = 0.0f;
};

/// Where terrain UV (u, v), clamped to [0, 1]^2, falls at level `level` of the pyramid `levels`
/// (BuildPageStoreLevels): level-0 sample i sits at i / (samples - 1), level-L sample j at
/// level-0 sample j * 2^L.
PageTexel LocatePageTexel(std::span<const PageStreaming::PageStoreLevel> levels, uint32 level, float32 u, float32 v);

/// The terrain UV along an axis of `samples` level-0 samples that page `page` of level `level` reads:
/// its owned samples and one sample of its level on each side, for the bilinear tap across its edge.
void PageAxisUV(uint32 page, uint32 level, uint32 samples, float32& outMin, float32& outMax);

/// A terrain-UV rect over which a terrain's page tables or page texels changed: a page assigned,
/// released or fading, a waiting page that took its entry, or an upload into a page's slot.
struct PageChangeRect
{
    uint32 Terrain = 0;                           ///< the terrain's render handle index
    Mathematics::Rect UV{0.0f, 0.0f, 1.0f, 1.0f}; ///< X along U, Y along V; the whole terrain by default
};

/// Brings `table`, stream `stream`'s page table, up to date with this frame of its field's `cache`:
/// the entries of the stream's pages that were assigned or released, and the fades of the slots of
/// its pages whose fade changed. Edge-triggered: a quiescent frame writes nothing. Returns the
/// number of entries and fades written.
uint32 UpdatePageTable(const PageStreaming::PageCache& cache, uint32 stream, PageTable& table);

/// Appends to `out` the footprint (PageAxisUV on both axes) of every page of stream `stream`, terrain
/// `terrain` (pyramid `levels`), that this frame of `cache` assigned, released, faded, rewrote or
/// uploaded, and of each page of `lateEntries` (the waiting pages that took their entry).
void AppendPageChangeRects(uint32 terrain, uint32 stream, std::span<const PageStreaming::PageStoreLevel> levels,
                           const PageStreaming::PageCache& cache,
                           std::span<const PageStreaming::PageAddress> lateEntries, std::vector<PageChangeRect>& out);

/// The resolve over a page table and the cache texture's texels (CacheDim x CacheDim floats).
struct PagedHeightSampler
{
    const PageTable* Table = nullptr;
    PageCacheGeometry Geometry;
    const float32* Cache = nullptr;

    /// The height at terrain UV (u, v) in [0, 1]^2 at continuous level `level`. Never reads outside
    /// a resident slot; 0 when not even the top level is resident.
    float32 Sample(float32 u, float32 v, float32 level) const;

    /// The height of level `level` alone at (u, v): the level's page when resident, faded in from
    /// its parent, else its first resident ancestor's. The resident level used goes to `outLevel`.
    float32 SampleLevel(float32 u, float32 v, uint32 level, uint32& outLevel) const;

private:
    float32 SampleResidentPage(const PageTexel& texel, uint32 slot) const;
};

} // namespace GameEngine::TerrainECS
