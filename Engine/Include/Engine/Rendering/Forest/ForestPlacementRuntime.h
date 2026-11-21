#pragma once

#include "Engine/Rendering/DrawCommand.h"
#include "Mathematics/WorldPosition.h"
#include "PageStreaming/Placement/PackedPlacementStore.h"
#include "PageStreaming/ResidentSlotPool.h"

#include <array>
#include <compare>
#include <memory>
#include <optional>
#include <utility>

namespace GameEngine::Engine::Renderer
{

struct ForestPageTicket
{
    uint32 Slot;
    uint32 Page;
    uint32 Generation;
    auto operator<=>(const ForestPageTicket&) const = default;
};

struct ForestPlacementRootBounds
{
    uint16 MinX;
    uint16 MinZ;
    uint16 MaxX;
    uint16 MaxZ;
};

struct ForestDetailDraw
{
    ForestPageTicket Ticket;
    uint32 FirstPlacement;
    uint32 PlacementCount;
};

struct ForestPlacementView
{
    Mathematics::WorldPosition Eye;
    std::array<Mathematics::Vector4, 6> EyeRelativePlanes;
    float DetailDistance = 0;
};

struct ForestPlacementFrame
{
    std::vector<DrawSubCommand> StandDraws;
    std::vector<ForestDetailDraw> DetailedDraws;
    std::vector<ForestPageTicket> Reads;
    std::vector<ForestPageTicket> CancelReads;
    uint32 VisibleCells = 0;
    bool WantsDetails = false;
};

/// Plans one forest's primary view on the render thread. The caller submits Reads
/// to AssetIO, acknowledges every read (including cancellation), and publishes a
/// successful upload only after its GPU completion. Stand geometry stays visible
/// until that publication. The store, geometry and runtime share one lifetime.
class ForestPlacementRuntime
{
public:
    /// standIndexOffsets is a prefix array of contiguous ranges, one per page
    /// plus the end offset. Zero slots configures stand-only rendering.
    static std::unique_ptr<ForestPlacementRuntime> Open(
        const std::filesystem::path& archive, std::span<const uint32> standIndexOffsets,
        uint32 slotCount, uint32 readsPerFrame, std::string& error);

    /// The cooked overview mesh stores one quad (six indices) per page.
    /// Transition slots let demand start reads while previous detail draws still own their slots.
    static std::shared_ptr<ForestPlacementRuntime> OpenQuadStand(
        const std::filesystem::path& archive, std::string& error, uint32 slots = 0, uint32 readsPerFrame = 0,
        bool retainPlacements = false, uint32 transitionSlots = 0);

    const PageStreaming::PackedPlacementStore& Store() const { return m_Store; }
    uint32 StandIndexEnd() const { return m_IndexOffsets.back(); }
    uint32 SlotCount() const { return static_cast<uint32>(m_Slots.size()); }

    /// Checks for a visible page within range before allocating the placement stream.
    bool CanRequestDetails(const ForestPlacementView& view) const;
    /// Checks for visible pages in range of the atlas and placement read lookahead.
    bool CanPrefetchDetails(const ForestPlacementView& view) const;

    /// Restart the stream after the owning graphics buffers have been replaced.
    void ResetResidency();

    /// completedReaders is the highest contiguous frame whose draws have finished.
    /// An invalid frame sequence clears the output and leaves residency unchanged.
    bool Prepare(const ForestPlacementView& view, uint64 frame, uint64 completedReaders, ForestPlacementFrame& out);

    /// Draw all visible stand cells, even when the primary view has detail.
    /// Does not change streaming residency or the completed-frame window.
    void GatherStand(const ForestPlacementView& view, std::vector<DrawSubCommand>& draws);

    /// True authorizes upload into ticket.Slot. Bytes may be empty on read failure.
    bool CompleteRead(ForestPageTicket ticket, std::span<const uint8> bytes);
    bool CompleteUpload(ForestPageTicket ticket, bool succeeded);
    bool CanRecordUpload(ForestPageTicket ticket) const;

    /// Optional CPU mirror of an admitted generation. Consume before the next Prepare or reset.
    std::span<const PageStreaming::PackedPlacement> ReadyPlacements(ForestPageTicket ticket) const;
    std::optional<ForestPlacementRootBounds> ReadyRootBounds(ForestPageTicket ticket) const;

private:
    friend class ForestPlacementStream;

    ForestPlacementRuntime(PageStreaming::PackedPlacementStore store, std::span<const uint32> offsets,
                           uint32 slots, uint32 readsPerFrame, bool retainPlacements = false,
                           uint32 transitionSlots = 0);

    enum class Stage { Empty, Reading, Uploading, Ready };
    struct Slot
    {
        ForestPageTicket Ticket{};
        Stage Current = Stage::Empty;
        uint64 LastReader = 0;
        bool Retiring = false;
        ForestPlacementRootBounds RootBounds{};
    };
    struct Bounds
    {
        std::array<double, 3> Min;
        std::array<double, 3> Max;
    };
    struct Node
    {
        Bounds Box;
        uint32 First;
        uint32 Count;
        uint32 Right = 0;
    };
    struct CellRun
    {
        uint32 First;
        uint32 Count;
    };

    uint32 BuildNodes(uint32 first, uint32 count);
    void Visit(uint32 node, const ForestPlacementView& view, const std::array<double, 3>& eye,
               uint32 planeMask, double detailSquared, std::vector<CellRun>& visible);
    static void AppendVisible(uint32 first, uint32 count, std::vector<CellRun>& visible);
    void DelayRetry(uint32 page);
    bool CanRequestWithin(const ForestPlacementView& view, double distance) const;
    bool HasVisibleDetailPage(uint32 index, const ForestPlacementView& view, const std::array<double, 3>& eye,
                              uint32 mask, double detailSquared) const;
    void GatherDemand(const ForestPlacementView& view);
    void ReleaseIfRetired(uint32 slot);
    void AppendReads(std::vector<ForestPageTicket>& reads);
    void EmitDraws(uint64 frame, ForestPlacementFrame& out);
    void EmitStand(uint32 first, uint32 end, std::vector<DrawSubCommand>& draws) const;

    PageStreaming::PackedPlacementStore m_Store;
    std::vector<uint32> m_IndexOffsets;
    std::vector<Bounds> m_Leaves;
    std::vector<Node> m_Nodes;
    std::vector<Slot> m_Slots;
    PageStreaming::ResidentSlotPool<uint32> m_Residency;
    std::vector<CellRun> m_Visible;
    std::vector<CellRun> m_StandVisible;
    std::optional<ForestPlacementView> m_DemandView;
    std::optional<ForestPlacementView> m_StandView;
    std::optional<uint64> m_DemandRetryFrame;
    std::vector<uint32> m_Priority;
    std::vector<uint8> m_Wanted;
    std::vector<uint64> m_RetryAt;
    std::vector<std::pair<double, uint32>> m_Candidates;
    std::vector<std::pair<uint32, uint32>> m_Ready;
    uint32 m_ReadsPerFrame;
    uint32 m_DetailCapacity;
    bool m_RetainPlacements = false;
    std::vector<PageStreaming::PackedPlacement> m_PlacementMirror;
    uint64 m_Frame = 0;
    uint64 m_ReadersDone = 0;
};

} // namespace GameEngine::Engine::Renderer
