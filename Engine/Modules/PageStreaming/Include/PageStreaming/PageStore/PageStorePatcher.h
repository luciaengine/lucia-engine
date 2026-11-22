#pragma once

#include "PageStreaming/PageAddress.h"
#include "PageStreaming/PageStore/PageStoreFormat.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

namespace JobSystem
{
class WorkStealingThreadPool;
}

namespace GameEngine::PageStreaming
{

class PageCodec;
class PageStoreWriter;
struct EncodedPage;

/// Writes the pages of a byte-format or Bc5 store and refilters the coarser pages that read them: a
/// coarser sample is the filter of the level below when every finer page its taps read is written, else
/// it keeps its stored value. WritePage runs on many threads; RefilterAncestors with no WritePage in flight.
/// A bake that writes its pages in many calls refilters once, after the last one: each coarser page is
/// then filtered once from its final finer pages.
class PageStorePatcher
{
public:
    /// Writes into `writer`. `pool` runs the filter and the encode; null runs them on this thread.
    PageStorePatcher(PageStoreWriter& writer, JobSystem::WorkStealingThreadPool* pool);
    /// Writes into the Bc5 store `writer` and into `unencoded`, an Rg8 store with the same lattice,
    /// filter and present pages. The coarser pages are filtered from `unencoded`, never from blocks.
    PageStorePatcher(PageStoreWriter& writer, PageStoreWriter& unencoded, JobSystem::WorkStealingThreadPool* pool);
    ~PageStorePatcher();

    PageStorePatcher(const PageStorePatcher&) = delete;
    PageStorePatcher& operator=(const PageStorePatcher&) = delete;

    /// Encodes one page's samples (PageStrideTexels(format)^2 texels of PageTexelBytes(format)
    /// bytes, row-major, apron included) and writes it at `address`. A page whose bytes the store
    /// already holds is not written. False on another format, a wrong sample count or a write error.
    bool WritePage(const PageAddress& address, std::span<const uint8> samples);

    /// Remakes, level by level, each coarser page that reads a sample of a page WritePage was given since the
    /// last call (every level-0 page, and each coarser page written with new bytes), each page written at its
    /// level, and each page that reads a page this call remade. A sample whose taps read a finer page not yet
    /// written keeps its stored value; a page with such a sample that is not written yet stays unwritten.
    /// False on a write error.
    bool RefilterAncestors();

    /// Pages written with new bytes, and pages whose bytes were already stored.
    uint64 PagesWritten() const { return m_PagesWritten.load(); }
    uint64 PagesUnchanged() const { return m_PagesUnchanged.load(); }

private:
    bool CanPatch() const;
    /// Writes one page's stride samples. False on a write error.
    bool StorePage(const PageAddress& address, const EncodedPage& encoded, const EncodedPage& unencoded);
    /// Encodes one page's stride samples for the store and, with an unencoded store, for it too.
    void EncodePage(uint32 level, std::span<const uint8> samples, EncodedPage& outEncoded,
                    EncodedPage& outUnencoded) const;
    /// The decoded stride samples the store holds for `address`; for a Bc5 store, the exact samples
    /// of its unencoded store where it has them. False when the page is not written.
    bool ReadStoredSamples(const PageAddress& address, std::vector<uint8>& outSamples) const;
    /// True when the store holds these bytes for `address`.
    bool HoldsPage(const PageAddress& address, const EncodedPage& encoded, const EncodedPage& unencoded) const;
    /// Remakes the pages of `level` that `marked` names (one byte a page of the level, row-major), a tile
    /// of pages at a time. Appends each page it rewrote to `outChanged`.
    bool RefilterMarked(uint32 level, std::span<const uint8> marked, std::vector<PageAddress>& outChanged);
    /// Remakes the pages of `level` in columns firstX to lastX and rows firstZ to lastZ whose byte in
    /// `remake` (one a page of the span, row-major) is set. Appends each page it rewrote to `outChanged`.
    bool RefilterSpan(uint32 level, uint32 firstX, uint32 lastX, uint32 firstZ, uint32 lastZ,
                      std::span<const uint8> remake, std::vector<PageAddress>& outChanged);

    PageStoreWriter& m_Writer;
    JobSystem::WorkStealingThreadPool* m_Pool = nullptr;
    std::unique_ptr<PageCodec> m_Codec;
    PageStoreWriter* m_Unencoded = nullptr; // a Bc5 store's Rg8 samples; null for a byte format
    std::unique_ptr<PageCodec> m_UnencodedCodec;
    std::atomic<uint64> m_PagesWritten{0};
    std::atomic<uint64> m_PagesUnchanged{0};
    std::mutex m_WrittenMutex;
    // One list a level, since the last RefilterAncestors: every level-0 page WritePage was given, and each
    // coarser page it wrote with new bytes.
    std::vector<std::vector<PageAddress>> m_Written;
};

} // namespace GameEngine::PageStreaming
