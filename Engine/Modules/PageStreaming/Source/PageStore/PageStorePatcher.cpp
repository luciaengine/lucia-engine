#include "PageStreaming/PageStore/PageStorePatcher.h"

#include "PageCodec.h"
#include "PageFieldFilter.h"
#include "PageStreaming/PageSampleRect.h"
#include "PageStreaming/PageStore/PageStoreWriter.h"

#include "JobSystem/WorkStealingThreadPool.h"
#include "Types/ByteBuffer.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace GameEngine::PageStreaming
{
namespace
{

// An inclusive span of samples or pages along one axis.
struct Span
{
    uint32 First = 0;
    uint32 Last = 0;

    uint32 Count() const { return Last - First + 1u; }
};

// The pages of a level whose stored samples hold a sample in `samples`. Page p stores the samples
// 128p - apron to 128p + 127 + apron.
Span PagesHolding(Span samples, uint32 pages, uint32 apron)
{
    const int64 low = static_cast<int64>(samples.First) - (kPageOwnedSamples - 1u) - apron;
    const uint32 first = low <= 0 ? 0u : static_cast<uint32>((low + kPageOwnedSamples - 1u) / kPageOwnedSamples);
    const uint32 last = std::min((samples.Last + apron) / kPageOwnedSamples, pages - 1u);
    return {first, last};
}

// The coarser samples that read a finer sample in `finer`: coarser sample j reads 2j - 1 to 2j + 1.
Span CoarserSamples(Span finer, uint32 coarserSamples)
{
    return {finer.First / 2u, std::min((finer.Last + 1u) / 2u, coarserSamples - 1u)};
}

// The finer samples that the stored samples of coarser page `page` read, clamped to the finer level.
Span FinerSamplesRead(uint32 page, uint32 apron, uint32 finerSamples)
{
    const int64 first = static_cast<int64>(page) * kPageOwnedSamples - apron;
    const int64 last = static_cast<int64>(page) * kPageOwnedSamples + kPageOwnedSamples - 1 + apron;
    return {ClampIndex(2 * first - 1, finerSamples), ClampIndex(2 * last + 1, finerSamples)};
}

// The pages that own the samples of `samples`.
Span OwningPages(Span samples)
{
    return {samples.First / kPageOwnedSamples, samples.Last / kPageOwnedSamples};
}

// The finer level's samples over a window, gathered from the owned samples of its stored pages.
struct FinerWindow
{
    Span X;
    Span Z;
    Span PagesX;
    Span PagesZ;
    uint32 TexelBytes = 0;
    std::vector<uint8> Texels;    // row-major over X by Z
    std::vector<uint8> PageReady; // one a page of PagesX by PagesZ: its samples are in Texels

    uint8* Texel(uint32 x, uint32 z)
    {
        return Texels.data() + (static_cast<std::size_t>(z - Z.First) * X.Count() + (x - X.First)) * TexelBytes;
    }
    const uint8* Texel(uint32 x, uint32 z) const { return const_cast<FinerWindow*>(this)->Texel(x, z); }

    bool PageIsReady(uint32 px, uint32 pz) const
    {
        return PageReady[static_cast<std::size_t>(pz - PagesZ.First) * PagesX.Count() + (px - PagesX.First)] != 0;
    }

    bool Ready(Span pagesX, Span pagesZ) const
    {
        for (uint32 pz = pagesZ.First; pz <= pagesZ.Last; ++pz)
            for (uint32 px = pagesX.First; px <= pagesX.Last; ++px)
                if (!PageIsReady(px, pz))
                    return false;
        return true;
    }

    bool AnyReady(Span pagesX, Span pagesZ) const
    {
        for (uint32 pz = pagesZ.First; pz <= pagesZ.Last; ++pz)
            for (uint32 px = pagesX.First; px <= pagesX.Last; ++px)
                if (PageIsReady(px, pz))
                    return true;
        return false;
    }
};

// The samples page `address` stores, clamped to its level.
PageSampleRect StoredSamples(const PageAddress& address, uint32 apron, const PageStoreLevel& shape)
{
    const int64 firstX = static_cast<int64>(address.X) * kPageOwnedSamples - apron;
    const int64 firstZ = static_cast<int64>(address.Z) * kPageOwnedSamples - apron;
    const int64 extent = kPageOwnedSamples - 1 + 2 * static_cast<int64>(apron);
    return {ClampIndex(firstX, shape.SamplesX), ClampIndex(firstZ, shape.SamplesZ),
            ClampIndex(firstX + extent, shape.SamplesX), ClampIndex(firstZ + extent, shape.SamplesZ)};
}

// The centered `toStride` square of a page's `fromStride` samples.
void CropSamples(std::span<const uint8> from, uint32 fromStride, uint32 toStride, uint32 texelBytes,
                 std::vector<uint8>& out)
{
    const uint32 inset = (fromStride - toStride) / 2u;
    const std::size_t rowBytes = static_cast<std::size_t>(toStride) * texelBytes;
    out.resize(rowBytes * toStride);
    for (uint32 r = 0; r < toStride; ++r)
        std::memcpy(out.data() + r * rowBytes,
                    from.data() + (static_cast<std::size_t>(r + inset) * fromStride + inset) * texelBytes, rowBytes);
}

enum class PageRemake : uint8
{
    Waiting,   // a page it reads is not written yet
    Unchanged, // the store holds these bytes
    Changed,
};

// Copies the owned samples of page (px, pz) of `level` that lie in `window`, decoded from `source`, a
// store whose pages have `sourceApron` samples of apron. False when the store does not hold the page.
bool GatherFinerPage(PageStoreWriter& source, const PageCodec& codec, uint32 level, uint32 px, uint32 pz,
                     uint32 sourceApron, FinerWindow& window)
{
    const PageAddress address{0, static_cast<uint8>(level), px, pz};
    PageIndexEntry entry;
    ByteBuffer stored;
    if (!source.ReadPage(address, stored, entry))
        return false;
    const uint32 sourceStride = kPageOwnedSamples + 2u * sourceApron;
    std::vector<uint8> samples(static_cast<std::size_t>(sourceStride) * sourceStride * window.TexelBytes);
    codec.Decode(level, entry, stored, samples);
    const uint32 x0 = std::max(px * kPageOwnedSamples, window.X.First);
    const uint32 x1 = std::min(px * kPageOwnedSamples + kPageOwnedSamples - 1u, window.X.Last);
    const uint32 z0 = std::max(pz * kPageOwnedSamples, window.Z.First);
    const uint32 z1 = std::min(pz * kPageOwnedSamples + kPageOwnedSamples - 1u, window.Z.Last);
    for (uint32 z = z0; z <= z1; ++z)
    {
        const std::size_t from = (static_cast<std::size_t>(z - pz * kPageOwnedSamples + sourceApron) * sourceStride +
                                  (x0 - px * kPageOwnedSamples + sourceApron)) *
                                 window.TexelBytes;
        std::memcpy(window.Texel(x0, z), samples.data() + from,
                    static_cast<std::size_t>(x1 - x0 + 1u) * window.TexelBytes);
    }
    return true;
}

// The finer pages that the 3 x 3 taps of coarser sample `sample` read.
Span TapPages(int64 sample, uint32 finerSamples)
{
    return OwningPages({ClampIndex(2 * sample - 1, finerSamples), ClampIndex(2 * sample + 1, finerSamples)});
}

// The stride samples of page (px, pz) of `level` of `layout`. A sample whose taps read only ready
// pages of `window` is their filter; another sample keeps its value in `inOutSamples`, the stored
// page. With `everyTapReady`, every sample is the filter and `inOutSamples` needs no value.
void FilterCoarserPage(const PageStoreLayout& layout, uint32 level, uint32 px, uint32 pz, const FinerWindow& window,
                       bool everyTapReady, std::vector<uint8>& inOutSamples)
{
    const uint32 stride = PageStrideTexels(layout.Header.Format);
    const uint32 apron = (stride - kPageOwnedSamples) / 2u;
    const PageStoreLevel& finer = layout.Levels[level - 1u];
    const PageStoreLevel& shape = layout.Levels[level];
    inOutSamples.resize(static_cast<std::size_t>(stride) * stride * window.TexelBytes);
    for (uint32 r = 0; r < stride; ++r)
    {
        const int64 sampleZ = ClampIndex(static_cast<int64>(pz) * kPageOwnedSamples - apron + r, shape.SamplesZ);
        const Span tapPagesZ = everyTapReady ? Span{} : TapPages(sampleZ, finer.SamplesZ);
        for (uint32 c = 0; c < stride; ++c)
        {
            const int64 sampleX = ClampIndex(static_cast<int64>(px) * kPageOwnedSamples - apron + c, shape.SamplesX);
            if (!everyTapReady && !window.Ready(TapPages(sampleX, finer.SamplesX), tapPagesZ))
                continue;
            FilterTaps taps{};
            for (uint32 dz = 0; dz < 3u; ++dz)
                for (uint32 dx = 0; dx < 3u; ++dx)
                    taps[dz * 3u + dx] = window.Texel(ClampIndex(2 * sampleX - 1 + dx, finer.SamplesX),
                                                      ClampIndex(2 * sampleZ - 1 + dz, finer.SamplesZ));
            FilterCoarserTexel(layout.Header.Filter, taps, window.TexelBytes,
                               inOutSamples.data() + (static_cast<std::size_t>(r) * stride + c) * window.TexelBytes);
        }
    }
}

// How many of the finer pages that a coarser page reads are ready in the window.
enum class FinerCoverage : uint8
{
    None, // or the coarser page is absent: no sample of it can change
    Some,
    All,
};

FinerCoverage CoverageOf(const PageStoreLayout& layout, const PageAddress& address, const FinerWindow& window)
{
    const uint32 apron = (PageStrideTexels(layout.Header.Format) - kPageOwnedSamples) / 2u;
    const PageStoreLevel& finer = layout.Levels[address.Level - 1u];
    const Span pagesX = OwningPages(FinerSamplesRead(address.X, apron, finer.SamplesX));
    const Span pagesZ = OwningPages(FinerSamplesRead(address.Z, apron, finer.SamplesZ));
    if (!layout.FindPresent(address) || !window.AnyReady(pagesX, pagesZ))
        return FinerCoverage::None;
    return window.Ready(pagesX, pagesZ) ? FinerCoverage::All : FinerCoverage::Some;
}

} // namespace

PageStorePatcher::PageStorePatcher(PageStoreWriter& writer, JobSystem::WorkStealingThreadPool* pool)
    : m_Writer(writer)
    , m_Pool(pool)
    , m_Codec(MakePageCodec(writer.Layout().Header))
    , m_Written(writer.Layout().Levels.size())
{
}

PageStorePatcher::PageStorePatcher(PageStoreWriter& writer, PageStoreWriter& unencoded,
                                   JobSystem::WorkStealingThreadPool* pool)
    : m_Writer(writer)
    , m_Pool(pool)
    , m_Codec(MakePageCodec(writer.Layout().Header))
    , m_Unencoded(&unencoded)
    , m_UnencodedCodec(MakePageCodec(unencoded.Layout().Header))
    , m_Written(writer.Layout().Levels.size())
{
}

PageStorePatcher::~PageStorePatcher() = default;

bool PageStorePatcher::CanPatch() const
{
    const PageStoreHeader& header = m_Writer.Layout().Header;
    if (!m_Codec)
        return false;
    if (!m_Unencoded)
        return header.Format == PageFieldFormat::Rgba8 || header.Format == PageFieldFormat::Rgba8Srgb ||
               header.Format == PageFieldFormat::Rg8;
    const PageStoreHeader& samples = m_Unencoded->Layout().Header;
    return header.Format == PageFieldFormat::Bc5 && m_UnencodedCodec && samples.Format == PageFieldFormat::Rg8 &&
           samples.Filter == header.Filter && samples.SamplesX == header.SamplesX && samples.SamplesZ == header.SamplesZ;
}

void PageStorePatcher::EncodePage(uint32 level, std::span<const uint8> samples, EncodedPage& outEncoded,
                                  EncodedPage& outUnencoded) const
{
    m_Codec->Encode(level, samples, PageFootprint{}, outEncoded);
    if (!m_Unencoded)
        return;
    std::vector<uint8> cropped;
    CropSamples(samples, PageStrideTexels(PageFieldFormat::Bc5), PageStrideTexels(PageFieldFormat::Rg8),
                PageTexelBytes(PageFieldFormat::Rg8), cropped);
    m_UnencodedCodec->Encode(level, cropped, PageFootprint{}, outUnencoded);
}

bool PageStorePatcher::ReadStoredSamples(const PageAddress& address, std::vector<uint8>& outSamples) const
{
    const PageFieldFormat format = m_Writer.Layout().Header.Format;
    const uint32 stride = PageStrideTexels(format);
    const uint32 texelBytes = PageTexelBytes(format);
    PageIndexEntry entry;
    ByteBuffer stored;
    if (!m_Writer.ReadPage(address, stored, entry))
        return false;
    outSamples.resize(static_cast<std::size_t>(stride) * stride * texelBytes);
    m_Codec->Decode(address.Level, entry, stored, outSamples);
    if (!m_Unencoded)
        return true;
    // The unencoded store holds the exact samples inside the Bc5 page's outer block ring.
    if (!m_Unencoded->ReadPage(address, stored, entry))
        return false;
    const uint32 exactStride = PageStrideTexels(m_Unencoded->Layout().Header.Format);
    std::vector<uint8> exact(static_cast<std::size_t>(exactStride) * exactStride * texelBytes);
    m_UnencodedCodec->Decode(address.Level, entry, stored, exact);
    const uint32 inset = (stride - exactStride) / 2u;
    const std::size_t rowBytes = static_cast<std::size_t>(exactStride) * texelBytes;
    for (uint32 r = 0; r < exactStride; ++r)
        std::memcpy(outSamples.data() + (static_cast<std::size_t>(r + inset) * stride + inset) * texelBytes,
                    exact.data() + r * rowBytes, rowBytes);
    return true;
}

bool PageStorePatcher::HoldsPage(const PageAddress& address, const EncodedPage& encoded,
                                 const EncodedPage& unencoded) const
{
    const PageIndexEntry* entry = m_Writer.Layout().FindPresent(address);
    if (!entry || entry->Offset == 0 || entry->Hash != encoded.Hash)
        return false;
    if (!m_Unencoded)
        return true;
    const PageIndexEntry* samples = m_Unencoded->Layout().FindPresent(address);
    return samples && samples->Offset != 0 && samples->Hash == unencoded.Hash;
}

bool PageStorePatcher::StorePage(const PageAddress& address, const EncodedPage& encoded, const EncodedPage& unencoded)
{
    return m_Writer.WritePage(address, encoded) && (!m_Unencoded || m_Unencoded->WritePage(address, unencoded));
}

bool PageStorePatcher::WritePage(const PageAddress& address, std::span<const uint8> samples)
{
    if (!CanPatch())
        return false;
    const PageFieldFormat format = m_Writer.Layout().Header.Format;
    const uint32 stride = PageStrideTexels(format);
    if (samples.size() != static_cast<std::size_t>(stride) * stride * PageTexelBytes(format))
        return false;
    if (!m_Writer.Layout().FindPresent(address) || (m_Unencoded && !m_Unencoded->Layout().FindPresent(address)))
        return false;
    EncodedPage encoded;
    EncodedPage unencoded;
    EncodePage(address.Level, samples, encoded, unencoded);
    const bool unchanged = HoldsPage(address, encoded, unencoded);
    if (unchanged)
        m_PagesUnchanged.fetch_add(1);
    else if (!StorePage(address, encoded, unencoded))
        return false;
    else
        m_PagesWritten.fetch_add(1);
    // A level-0 page is a change whatever its bytes: its ancestors can still hold another bake.
    if (address.Level == 0u || !unchanged)
    {
        std::lock_guard lock(m_WrittenMutex);
        m_Written[address.Level].push_back(address);
    }
    return true;
}

bool PageStorePatcher::RefilterAncestors()
{
    if (!CanPatch())
        return false;
    std::vector<std::vector<PageAddress>> written(m_Written.size());
    {
        std::lock_guard lock(m_WrittenMutex);
        written.swap(m_Written);
    }
    const PageStoreLayout& layout = m_Writer.Layout();
    const uint32 apron = (PageStrideTexels(layout.Header.Format) - kPageOwnedSamples) / 2u;
    const uint32 levels = static_cast<uint32>(std::min(layout.Levels.size(), written.size()));
    // The samples of the finer level's changed pages: each level-0 page WritePage was given with the
    // lattice's one-sample apron, then at each level the stored samples of the pages written and remade.
    std::vector<PageSampleRect> changed;
    if (!written.empty())
        for (const PageAddress& address : written.front())
            changed.push_back(StoredSamples(address, kPageApronSamples, layout.Levels.front()));
    for (uint32 level = 1; level < levels; ++level)
    {
        const PageStoreLevel& shape = layout.Levels[level];
        std::vector<uint8> marked(static_cast<std::size_t>(shape.PagesX) * shape.PagesZ, 0u);
        for (const PageSampleRect& samples : changed)
        {
            const Span x = PagesHolding(CoarserSamples({samples.MinX, samples.MaxX}, shape.SamplesX), shape.PagesX, apron);
            const Span z = PagesHolding(CoarserSamples({samples.MinZ, samples.MaxZ}, shape.SamplesZ), shape.PagesZ, apron);
            for (uint32 pz = z.First; pz <= z.Last; ++pz)
                std::fill_n(marked.begin() + static_cast<std::ptrdiff_t>(pz) * shape.PagesX + x.First, x.Count(), 1u);
        }
        for (const PageAddress& address : written[level])
            marked[static_cast<std::size_t>(address.Z) * shape.PagesX + address.X] = 1u;
        std::vector<PageAddress> remade;
        if (!RefilterMarked(level, marked, remade))
            return false;
        // A page WritePage wrote is a new input to the level above, whatever this call made of it.
        changed.clear();
        for (const PageAddress& address : written[level])
            changed.push_back(StoredSamples(address, apron, shape));
        for (const PageAddress& address : remade)
            changed.push_back(StoredSamples(address, apron, shape));
    }
    return true;
}

bool PageStorePatcher::RefilterMarked(uint32 level, std::span<const uint8> marked,
                                      std::vector<PageAddress>& outChanged)
{
    // A tile bounds the finer window that one span gathers: kRefilterTilePages pages a side read about
    // twice that many finer pages a side.
    constexpr uint32 kRefilterTilePages = 8u;
    const PageStoreLevel& shape = m_Writer.Layout().Levels[level];
    std::vector<uint8> remake;
    for (uint32 tileZ = 0; tileZ < shape.PagesZ; tileZ += kRefilterTilePages)
        for (uint32 tileX = 0; tileX < shape.PagesX; tileX += kRefilterTilePages)
        {
            const uint32 endX = std::min(tileX + kRefilterTilePages, shape.PagesX);
            const uint32 endZ = std::min(tileZ + kRefilterTilePages, shape.PagesZ);
            uint32 firstX = endX;
            uint32 firstZ = endZ;
            uint32 lastX = 0;
            uint32 lastZ = 0;
            for (uint32 pz = tileZ; pz < endZ; ++pz)
                for (uint32 px = tileX; px < endX; ++px)
                    if (marked[static_cast<std::size_t>(pz) * shape.PagesX + px] != 0u)
                    {
                        firstX = std::min(firstX, px);
                        firstZ = std::min(firstZ, pz);
                        lastX = std::max(lastX, px);
                        lastZ = std::max(lastZ, pz);
                    }
            if (firstX == endX)
                continue;
            remake.clear();
            for (uint32 pz = firstZ; pz <= lastZ; ++pz)
                for (uint32 px = firstX; px <= lastX; ++px)
                    remake.push_back(marked[static_cast<std::size_t>(pz) * shape.PagesX + px]);
            if (!RefilterSpan(level, firstX, lastX, firstZ, lastZ, remake, outChanged))
                return false;
        }
    return true;
}

bool PageStorePatcher::RefilterSpan(uint32 level, uint32 firstX, uint32 lastX, uint32 firstZ, uint32 lastZ,
                                    std::span<const uint8> remake, std::vector<PageAddress>& outChanged)
{
    const PageStoreLayout& layout = m_Writer.Layout();
    const PageFieldFormat format = layout.Header.Format;
    const uint32 stride = PageStrideTexels(format);
    const uint32 apron = (stride - kPageOwnedSamples) / 2u;
    const uint32 texelBytes = PageTexelBytes(format);
    const PageStoreLevel& finer = layout.Levels[level - 1u];
    const Span pagesX{firstX, lastX};
    const Span pagesZ{firstZ, lastZ};

    // The finer samples every remade page reads, from the pages that own them: a Bc5 store's from
    // its unencoded store.
    PageStoreWriter& source = m_Unencoded ? *m_Unencoded : m_Writer;
    const PageCodec& sourceCodec = m_Unencoded ? *m_UnencodedCodec : *m_Codec;
    const uint32 sourceStride = PageStrideTexels(source.Layout().Header.Format);
    const uint32 sourceApron = (sourceStride - kPageOwnedSamples) / 2u;
    FinerWindow window;
    window.TexelBytes = texelBytes;
    window.X = {FinerSamplesRead(pagesX.First, apron, finer.SamplesX).First,
                FinerSamplesRead(pagesX.Last, apron, finer.SamplesX).Last};
    window.Z = {FinerSamplesRead(pagesZ.First, apron, finer.SamplesZ).First,
                FinerSamplesRead(pagesZ.Last, apron, finer.SamplesZ).Last};
    window.PagesX = OwningPages(window.X);
    window.PagesZ = OwningPages(window.Z);
    window.Texels.resize(static_cast<std::size_t>(window.X.Count()) * window.Z.Count() * texelBytes);
    window.PageReady.assign(static_cast<std::size_t>(window.PagesX.Count()) * window.PagesZ.Count(), 0u);
    JobSystem::ParallelFor(m_Pool, window.PageReady.size(), [&](std::size_t i) {
        const uint32 px = window.PagesX.First + static_cast<uint32>(i % window.PagesX.Count());
        const uint32 pz = window.PagesZ.First + static_cast<uint32>(i / window.PagesX.Count());
        window.PageReady[i] = GatherFinerPage(source, sourceCodec, level - 1u, px, pz, sourceApron, window) ? 1u : 0u;
    });

    const std::size_t remakeCount = static_cast<std::size_t>(pagesX.Count()) * pagesZ.Count();
    std::vector<EncodedPage> encoded(remakeCount);
    std::vector<EncodedPage> unencoded(remakeCount);
    std::vector<PageRemake> outcome(remakeCount, PageRemake::Waiting);
    JobSystem::ParallelFor(m_Pool, remakeCount, [&](std::size_t i) {
        if (remake[i] == 0u)
            return;
        const uint32 px = pagesX.First + static_cast<uint32>(i % pagesX.Count());
        const uint32 pz = pagesZ.First + static_cast<uint32>(i / pagesX.Count());
        const PageAddress address{0, static_cast<uint8>(level), px, pz};
        const FinerCoverage coverage = CoverageOf(layout, address, window);
        std::vector<uint8> samples;
        if (coverage == FinerCoverage::None || (coverage == FinerCoverage::Some && !ReadStoredSamples(address, samples)))
            return;
        FilterCoarserPage(layout, level, px, pz, window, coverage == FinerCoverage::All, samples);
        EncodePage(level, samples, encoded[i], unencoded[i]);
        outcome[i] = HoldsPage(address, encoded[i], unencoded[i]) ? PageRemake::Unchanged : PageRemake::Changed;
    });

    // Written in address order, so the store's bytes do not depend on the workers.
    for (std::size_t i = 0; i < remakeCount; ++i)
    {
        if (outcome[i] == PageRemake::Unchanged)
            m_PagesUnchanged.fetch_add(1);
        if (outcome[i] != PageRemake::Changed)
            continue;
        const PageAddress address{0, static_cast<uint8>(level), pagesX.First + static_cast<uint32>(i % pagesX.Count()),
                                  pagesZ.First + static_cast<uint32>(i / pagesX.Count())};
        if (!StorePage(address, encoded[i], unencoded[i]))
            return false;
        m_PagesWritten.fetch_add(1);
        outChanged.push_back(address);
    }
    return true;
}

} // namespace GameEngine::PageStreaming
