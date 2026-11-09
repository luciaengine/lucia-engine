// Sparse height deltas applied to a tiled terrain's base pages.

#include "TerrainECS/TerrainModifierSystem.h"

#include "PageStreaming/Height/HeightPageLayer.h"
#include "PageStreaming/PageSampleRect.h"
#include "PageStreaming/PageStore/PageStoreFormat.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace GameEngine::TerrainECS
{

namespace
{

using PageStreaming::PageSampleRect;

// The terrain's global tile lattice: level 0 of its height pages.
struct TileLattice
{
    uint32 SamplesX = 0;
    uint32 SamplesZ = 0;
    float32 SpacingX = 0.0f;
    float32 SpacingZ = 0.0f;
    float32 OriginX = 0.0f;
    float32 OriginZ = 0.0f;
};

std::optional<TileLattice> MakeTileLattice(const TiledTerrainData& tiled)
{
    const uint32 tileW = tiled.Config.TileConfig.HeightmapWidth;
    const uint32 tileH = tiled.Config.TileConfig.HeightmapHeight;
    if (tileW < 2u || tileH < 2u || tiled.Config.TilesPerAxisX == 0u || tiled.Config.TilesPerAxisZ == 0u ||
        !(tiled.Config.TileWorldSize > 0.0f))
        return std::nullopt;
    TileLattice lattice;
    lattice.SamplesX = tiled.Config.TilesPerAxisX * (tileW - 1u) + 1u;
    lattice.SamplesZ = tiled.Config.TilesPerAxisZ * (tileH - 1u) + 1u;
    lattice.SpacingX = tiled.Config.TileWorldSize / static_cast<float32>(tileW - 1u);
    lattice.SpacingZ = tiled.Config.TileWorldSize / static_cast<float32>(tileH - 1u);
    lattice.OriginX = tiled.WorldOriginX;
    lattice.OriginZ = tiled.WorldOriginZ;
    return lattice;
}

// The lattice samples a world rect reaches, one sample of margin on each side (the bake's own
// sample mapping rounds both ways), or nothing when it misses the terrain.
std::optional<PageSampleRect> LatticeRect(const TileLattice& lattice, float32 minX, float32 minZ, float32 maxX,
                                          float32 maxZ)
{
    const float64 firstX = std::floor((minX - lattice.OriginX) / lattice.SpacingX) - 1.0;
    const float64 firstZ = std::floor((minZ - lattice.OriginZ) / lattice.SpacingZ) - 1.0;
    const float64 lastX = std::ceil((maxX - lattice.OriginX) / lattice.SpacingX) + 1.0;
    const float64 lastZ = std::ceil((maxZ - lattice.OriginZ) / lattice.SpacingZ) + 1.0;
    if (lastX < 0.0 || lastZ < 0.0 || firstX > lattice.SamplesX - 1.0 || firstZ > lattice.SamplesZ - 1.0)
        return std::nullopt;
    PageSampleRect rect;
    rect.MinX = static_cast<uint32>(std::max(firstX, 0.0));
    rect.MinZ = static_cast<uint32>(std::max(firstZ, 0.0));
    rect.MaxX = static_cast<uint32>(std::min(lastX, lattice.SamplesX - 1.0));
    rect.MaxZ = static_cast<uint32>(std::min(lastZ, lattice.SamplesZ - 1.0));
    return rect;
}

// True when rects `a` and `b` overlap or share a side.
bool RectsTouch(const PageSampleRect& a, const PageSampleRect& b)
{
    return a.MinX <= b.MaxX + 1u && b.MinX <= a.MaxX + 1u && a.MinZ <= b.MaxZ + 1u && b.MinZ <= a.MaxZ + 1u;
}

// `rects` with every pair that overlaps or touches replaced by their union, so the bands that compose
// them never compose one sample twice.
std::vector<PageSampleRect> MergeTouching(std::vector<PageSampleRect> rects)
{
    // Each pass grows a rect by every rect it touches; a grown rect can touch one it passed, so passes
    // repeat until none merges.
    for (bool merged = true; merged;)
    {
        merged = false;
        for (std::size_t kept = 0; kept < rects.size(); ++kept)
            for (std::size_t other = kept + 1; other < rects.size();)
            {
                if (!RectsTouch(rects[kept], rects[other]))
                {
                    ++other;
                    continue;
                }
                rects[kept] = Union(rects[kept], rects[other]);
                rects[other] = rects.back();
                rects.pop_back();
                merged = true;
            }
    }
    return rects;
}

// The level-0 bounds of what `layer` holds, or an empty rect at the origin when it holds nothing.
PageSampleRect ExtentOf(const PageStreaming::HeightPageLayer& layer)
{
    return layer.Extent(0).value_or(PageSampleRect{});
}

// Publishes the layer and its changed level-0 rect. Versions are unique across terrains, so
// resident pages distinguish an incremental write from rebuilt modifier data after a scene load.
void PublishPageOverlay(TiledTerrainData& tiled, std::shared_ptr<PageStreaming::HeightPageLayer> layer,
                        const PageSampleRect& changed)
{
    static std::atomic<uint64> s_LastVersion{0};
    tiled.PageOverlay = std::move(layer);
    tiled.PageOverlayChanged = changed;
    tiled.PageOverlayChangedSince = tiled.PageOverlayVersion;
    tiled.PageOverlayVersion = ++s_LastVersion;
}

// Drops the terrain's overlay: it pages no more until a new one is built.
void ReleaseHeightPageOverlay(TiledTerrainData& tiled)
{
    if (tiled.PageOverlay)
        PublishPageOverlay(tiled, nullptr, ExtentOf(*tiled.PageOverlay));
    tiled.PageOverlayCurrent = false;
    tiled.PageOverlayRefusedBytes = 0;
    tiled.PageOverlayRefusedOffGrid = false;
}

} // namespace

void TerrainModifierSystem::BakeHeightPageOverlay(TiledTerrainData& tiled, float32 heightScale,
                                                  float32 terrainOriginY,
                                                  const std::vector<ResolvedModifier>& modifiers, bool fullBake,
                                                  const DirtyUnion& heightDirty)
{
    const std::optional<TileLattice> lattice = MakeTileLattice(tiled);
    if (!lattice || tiled.PageOverlayByteCap == 0)
    {
        // A terrain that keeps its height texture holds no overlay; it is built when it would page.
        ReleaseHeightPageOverlay(tiled);
        return;
    }
    std::vector<PageSampleRect> reach;
    for (const ResolvedModifier& modifier : modifiers)
    {
        if (modifier.IsSplatOnlyModifier())
            continue;
        if (const auto rect = LatticeRect(*lattice, modifier.BoundsMinX, modifier.BoundsMinZ, modifier.BoundsMaxX,
                                          modifier.BoundsMaxZ))
            reach.push_back(*rect);
    }
    PageStreaming::HeightPageLayer* previous = tiled.PageOverlay.get();
    if (reach.empty())
    {
        // No height modifier reaches the terrain: its pages are its base.
        if (previous)
            PublishPageOverlay(tiled, nullptr, ExtentOf(*previous));
        tiled.PageOverlayCurrent = true;
        tiled.PageOverlayRefusedBytes = 0;
        tiled.PageOverlayRefusedOffGrid = false;
        return;
    }
    if (tiled.PageOverlayOffGrid)
    {
        // Its pages are not on the lattice the overlay is baked on: the terrain keeps its height
        // texture while a modifier reaches it (the driver logs why).
        ReleaseHeightPageOverlay(tiled);
        tiled.PageOverlayRefusedOffGrid = true;
        return;
    }

    const std::vector<PageStreaming::PageStoreLevel> levels =
        PageStreaming::BuildPageStoreLevels(lattice->SamplesX, lattice->SamplesZ);
    reach = MergeTouching(std::move(reach));
    const uint64 bytes = PageStreaming::HeightPageLayerBytesOver(levels, reach);
    if (bytes > tiled.PageOverlayByteCap)
    {
        // Over the platform's cap: the terrain keeps its height texture (the driver logs why).
        ReleaseHeightPageOverlay(tiled);
        tiled.PageOverlayRefusedBytes = bytes;
        return;
    }
    tiled.PageOverlayRefusedBytes = 0;

    // An edit writes the dirty rect into the layer it holds: a sample outside the rect keeps its
    // delta, and a sample a modifier left or reached holds the new delta (zero where none reaches).
    std::optional<PageSampleRect> dirty;
    if (heightDirty.Any)
        dirty = LatticeRect(*lattice, heightDirty.MinX, heightDirty.MinZ, heightDirty.MaxX, heightDirty.MaxZ);
    const bool edit = !fullBake && previous && tiled.PageOverlayCurrent && previous->FitsLevels(levels);
    if (edit && !dirty)
        return; // nothing under the terrain's height changed
    const std::vector<PageSampleRect> writes = edit ? std::vector<PageSampleRect>{*dirty} : reach;

    std::vector<BakeBandRegion> regions;
    std::vector<std::vector<float32>> deltas(writes.size());
    for (std::size_t region = 0; region < writes.size(); ++region)
    {
        const PageSampleRect& write = writes[region];
        regions.push_back(BakeBandRegion{static_cast<int32>(write.MinX), static_cast<int32>(write.MinZ),
                                         static_cast<int32>(write.MaxX), static_cast<int32>(write.MaxZ)});
        deltas[region].assign(static_cast<std::size_t>(write.Width()) * write.Height(), 0.0f);
    }
    RunBakeRowBands(regions, [this, &tiled, &modifiers, &writes, &deltas, heightScale, terrainOriginY](
                                 std::size_t region, int32 firstX, int32 lastX, int32 firstZ, int32 lastZ) {
        ComposeHeightPageDeltaBand(tiled, heightScale, terrainOriginY, modifiers, writes[region],
                                   firstX, lastX, firstZ, lastZ, deltas[region]);
    });

    if (edit)
    {
        previous->WriteLevel0(0, *dirty, deltas.front());
        PublishPageOverlay(tiled, tiled.PageOverlay, *dirty);
        if (previous->Bytes() > tiled.PageOverlayByteCap)
        {
            const uint64 held = previous->Bytes();
            ReleaseHeightPageOverlay(tiled);
            tiled.PageOverlayRefusedBytes = held;
            return;
        }
    }
    else
    {
        // A rebuild changes what either layer holds.
        auto layer = std::make_shared<PageStreaming::HeightPageLayer>(1u, levels);
        for (std::size_t region = 0; region < writes.size(); ++region)
            layer->WriteLevel0(0, writes[region], deltas[region]);
        std::optional<PageSampleRect> changed = layer->Extent(0);
        for (const std::optional<PageSampleRect>& more : {previous ? previous->Extent(0) : std::nullopt, dirty})
            if (more)
                changed = changed ? Union(*changed, *more) : *more;
        PublishPageOverlay(tiled, std::move(layer), changed.value_or(PageSampleRect{}));
    }
    tiled.PageOverlayCurrent = true;
}

void TerrainModifierSystem::ComposeHeightPageDeltaBand(const TiledTerrainData& tiled, float32 heightScale,
                                                        float32 terrainOriginY,
                                                        const std::vector<ResolvedModifier>& modifiers,
                                                        const PageStreaming::PageSampleRect& write,
                                                        int32 firstX, int32 lastX, int32 firstZ, int32 lastZ,
                                                        std::span<float32> deltas)
{
    static const std::vector<ResolvedModifier> kNoModifiers;
    const uint32 countX = static_cast<uint32>(lastX - firstX + 1);
    const uint32 countZ = static_cast<uint32>(lastZ - firstZ + 1);
    std::vector<float32> baked;
    std::vector<float32> base;
    if (!ComposeLatticeBlock(tiled, heightScale, terrainOriginY, modifiers, firstX, firstZ, countX, countZ, baked) ||
        !ComposeLatticeBlock(tiled, heightScale, terrainOriginY, kNoModifiers, firstX, firstZ, countX, countZ, base))
        return;
    for (uint32 row = 0; row < countZ; ++row)
    {
        const std::size_t destination =
            static_cast<std::size_t>(firstZ - static_cast<int32>(write.MinZ) + row) * write.Width() +
            static_cast<std::size_t>(firstX - static_cast<int32>(write.MinX));
        for (uint32 column = 0; column < countX; ++column)
            deltas[destination + column] = baked[static_cast<std::size_t>(row) * countX + column] -
                                           base[static_cast<std::size_t>(row) * countX + column];
    }
}

} // namespace GameEngine::TerrainECS
