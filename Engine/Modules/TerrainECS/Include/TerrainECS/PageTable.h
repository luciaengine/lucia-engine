#pragma once

#include "PageStreaming/PageAddress.h"
#include "PageStreaming/PageStore/PageStoreFormat.h"
#include "Types/Types.h"

#include <span>
#include <vector>

// Maps resident height pages to cache slots. Small fields use dense words; larger fields allocate
// bounded blocks below a dense floor. The CPU mirror and cbt_page.glsl walk the same packed words.
namespace GameEngine::TerrainECS
{

/// What one field's page table may spend.
struct PageTableBudget
{
    uint64 DenseWords = 0; ///< a table whose dense words (binding-22 header, fades, entries) fit keeps one tier
    uint32 MidBlocks = 0;  ///< the blocks of each tier but the last
    uint32 LeafBlocks = 0; ///< the blocks of the last tier
};

class PageTable
{
public:
    /// An empty table (no page resident) for a field of `faces` faces (1 or 6) of samplesX x samplesZ
    /// level-0 samples each and a cache of slotCount slots.
    void Reset(uint32 samplesX, uint32 samplesZ, uint32 faces, uint32 slotCount, const PageTableBudget& budget);

    /// The entry of a page: PackPageEntry(slot, level), or kNoPage.
    uint32 Entry(const PageStreaming::PageAddress& page) const;
    /// Sets the entry of a page. A resident page below the floor whose pool is full waits, reading
    /// kNoPage (its first resident ancestor answers), until a block frees.
    void SetEntry(const PageStreaming::PageAddress& page, uint32 entry);
    void SetSlotFade(uint32 slot, float32 fade) { m_SlotFade[slot] = fade; }

    /// Appends every page with an entry.
    void AppendResident(std::vector<PageStreaming::PageAddress>& out) const;
    /// Moves out the waiting pages that took their entry since the last call.
    void TakeLateEntries(std::vector<PageStreaming::PageAddress>& out);

    std::span<const PageStreaming::PageStoreLevel> Levels() const { return m_Levels; }
    /// Each level's first word: its dense entries, or its offset inside a block.
    std::span<const uint32> LevelOffsets() const { return m_LevelOffsets; }
    std::span<const uint32> Words() const { return m_Words; }
    std::span<const float32> SlotFades() const { return m_SlotFade; }
    /// The finest dense level: 0 for a dense table.
    uint32 FloorLevel() const { return m_FloorLevel; }
    uint32 DirectoryOffset() const { return m_DirectoryOffset; }
    /// The tiers of blocks below the floor: 0 for a dense table.
    uint32 TierCount() const { return static_cast<uint32>(m_Tiers.size()); }

private:
    /// One tier of blocks below the dense tier.
    struct Tier
    {
        uint32 RootLevel = 0;  ///< the level of the page a block hangs under
        uint32 Levels = 0;     ///< RootLevel - 1 down to RootLevel - Levels
        uint32 BlockWords = 0; ///< the levels' entries, then the next tier's directory when one follows
        uint32 PoolOffset = 0; ///< the pool's first word
        uint32 Blocks = 0;
    };

    struct Pool
    {
        std::vector<uint32> Free;
        std::vector<uint32> Residents; ///< per block: its resident entries and its allocated child blocks
        std::vector<uint32> Owner;     ///< per block: the directory word that points at it
        std::vector<uint32> Parent;    ///< per block: its parent block in the tier above (kNoPageBlock: the floor's)
        std::vector<uint32> RootX;     ///< per block: the page it hangs under, at its tier's root level
        std::vector<uint32> RootZ;
        std::vector<uint8> RootFace;
    };

    struct Waiting
    {
        PageStreaming::PageAddress Address;
        uint32 Entry = 0;
    };

    uint32 DenseWord(const PageStreaming::PageAddress& page) const;
    uint32 FloorDirectoryWord(const PageStreaming::PageAddress& page) const;
    /// The word of `tier`'s block `block` on the walk to a page: its entry when the page's level is in
    /// the block, else the next tier's directory word. (rootX, rootZ) is the page at the tier's root
    /// level that the block hangs under.
    uint32 BlockWord(const Tier& tier, uint32 block, uint32 rootX, uint32 rootZ, uint32 level, uint32 pageX,
                     uint32 pageZ) const;
    bool Insert(const PageStreaming::PageAddress& page, uint32 entry);
    void Remove(const PageStreaming::PageAddress& page);
    void Release(uint32 tier, uint32 block);
    void RetryWaiting();

    std::vector<PageStreaming::PageStoreLevel> m_Levels;
    uint32 m_Faces = 1;
    std::vector<uint32> m_LevelOffsets;
    std::vector<uint32> m_Words;
    std::vector<float32> m_SlotFade;
    uint32 m_FloorLevel = 0;
    uint32 m_DirectoryOffset = 0;
    std::vector<Tier> m_Tiers;
    std::vector<Pool> m_Pools;
    std::vector<Waiting> m_Waiting;
    std::vector<PageStreaming::PageAddress> m_LateEntries;
};

} // namespace GameEngine::TerrainECS
