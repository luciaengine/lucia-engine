#pragma once

#include "Engine/Rendering/DrawCommand.h"
#include "Engine/Rendering/Forest/ForestDetailBudget.h"
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

/// One admission of an archive page into a placement slot. Each admission of a slot has its own
/// generation, so the ticket of an earlier admission does not match the slot.
struct ForestPageTicket
{
    uint32 Slot;
    uint32 Page;
    uint32 Generation;
    auto operator<=>(const ForestPageTicket&) const = default;
};

/// The range of a page's packed root offsets, in centimeters from the page origin.
struct ForestPlacementRootBounds
{
    uint16 MinX;
    uint16 MinY;
    uint16 MinZ;
    uint16 MaxX;
    uint16 MaxY;
    uint16 MaxZ;
};

/// The resident placements of one page: FirstPlacement indexes the slot buffer
/// (slot * PackedPlacementStore::kPlacementsPerPage).
struct ForestDetailDraw
{
    ForestPageTicket Ticket;
    uint32 FirstPlacement;
    uint32 PlacementCount;
    /// The share of the page's pixels that its trees draw, in (0, 1]. Below 1 the page is in the
    /// handoff band, and its overview quad draws the other pixels (ForestPlacementFrame::StandFades).
    float DetailWeight = 1.0f;
};

/// The overview quad of a page in the handoff band, with the share of the page's pixels that its trees draw.
struct ForestStandFade
{
    DrawSubCommand Range;
    float DetailWeight;
};

/// The view that a forest is planned for.
struct ForestPlacementView
{
    Mathematics::WorldPosition Eye;
    /// Frustum planes relative to Eye, normals toward the inside (Rendering::TestAabbFrustum).
    std::array<Mathematics::Vector4, 6> EyeRelativePlanes;
    /// Meters from the eye within which visible pages draw their trees. 0 draws the overview only.
    float DetailDistance = 0;
    /// Meters past DetailDistance over which the trees fade into the overview. 0 switches at DetailDistance.
    float HandoffWidth = 0;
};

/// The plan of one frame for one view.
struct ForestPlacementFrame
{
    /// Overview index ranges of the visible pages that do not draw their trees.
    std::vector<DrawSubCommand> StandDraws;
    /// Visible pages whose placements are resident.
    std::vector<ForestDetailDraw> DetailedDraws;
    /// Overview quads of the detailed pages in the handoff band. StandDraws does not hold them.
    std::vector<ForestStandFade> StandFades;
    ForestDetailBudget DetailBudget;
    /// New reads for the caller to submit.
    std::vector<ForestPageTicket> Reads;
    /// Reads that the runtime cancelled; the caller withdraws them from the asset reader.
    std::vector<ForestPageTicket> CancelReads;
    /// Visible pages, drawn as overview or as trees.
    uint32 VisiblePages = 0;
    /// True when a visible page is within the detail or prefetch distance.
    bool WantsDetails = false;
};

/// How an upload of a page into its slot ended.
enum class ForestUploadResult : uint8
{
    Published, // The copy finished on the GPU: the slot draws.
    Failed,    // The copy failed: the page waits for its retry frame.
    Abandoned, // The frame did not submit: the page can be read again at once.
};

/// The size of a runtime's slot set.
struct ForestPlacementRuntimeDesc
{
    /// Slots for pages that draw their trees. 0 draws the overview mesh only.
    uint32 DetailSlots = 0;
    /// Extra slots, so that demand can start reads while earlier draws still own their slots.
    uint32 TransitionSlots = 0;
    /// The most page reads that one frame starts. 0 draws the overview mesh only.
    uint32 ReadsPerFrame = 0;
    /// Keep a CPU copy of each resident page for ReadyPlacements.
    bool KeepCpuCopy = false;
};

/// Plans one forest's primary view: page culling, overview cover, page residency in a fixed set of
/// slots, and the reads that fill them. The caller submits Reads to the asset reader, reports every
/// read with CompleteRead, and reports each upload with CompleteUpload after its GPU fence. A page
/// draws its overview quad until its upload is published. A slot is reused only after the frame
/// that last drew from it is complete. All members run on the render thread, one call at a time.
class ForestPlacementRuntime
{
public:
    /// Opens a .geforest archive whose overview mesh has one quad (six indices) per page, in page order.
    /// Returns null and sets error when the archive or the slot set is not valid.
    static std::shared_ptr<ForestPlacementRuntime> Open(const std::filesystem::path& archive,
                                                        const ForestPlacementRuntimeDesc& desc, std::string& error);

    /// The archive directory.
    const PageStreaming::PackedPlacementStore& Store() const { return m_Store; }
    /// The index count of the whole overview mesh.
    uint32 StandIndexEnd() const { return static_cast<uint32>(m_Store.Pages().size()) * kIndicesPerPage; }
    /// The number of slots, detail and transition together.
    uint32 SlotCount() const { return static_cast<uint32>(m_Slots.size()); }

    /// True when a visible page is within the prefetch distance (1.4 times the detail and handoff distance).
    bool CanPrefetchDetails(const ForestPlacementView& view) const;

    /// Forgets every slot and read. Call it after the slot buffer and its uploads are gone.
    void ResetResidency();

    /// Plans `frame`. completedReaders is the highest frame whose draws are complete on the GPU.
    /// Frames must increase and completedReaders must be below frame; otherwise it returns false,
    /// clears the output and leaves residency unchanged.
    bool Prepare(const ForestPlacementView& view, uint64 frame, uint64 completedReaders, ForestPlacementFrame& out);

    /// The overview ranges of every visible page of another view, such as a shadow or a second camera.
    /// It does not change residency.
    void GatherStand(const ForestPlacementView& view, std::vector<DrawSubCommand>& draws);

    /// Reports a read. Empty or corrupt bytes fail it. True means the caller uploads the bytes into ticket.Slot.
    bool CompleteRead(ForestPageTicket ticket, std::span<const uint8> bytes);
    /// Reports an upload. Returns true when the slot draws from now on.
    bool CompleteUpload(ForestPageTicket ticket, ForestUploadResult result);
    /// True while the ticket's read is verified and its upload is not reported.
    bool CanRecordUpload(ForestPageTicket ticket) const;

    /// The CPU copy of a resident page (KeepCpuCopy), valid until the next Prepare or ResetResidency.
    std::span<const PageStreaming::PackedPlacement> ReadyPlacements(ForestPageTicket ticket) const;
    /// The root offset range of a resident page.
    std::optional<ForestPlacementRootBounds> ReadyRootBounds(ForestPageTicket ticket) const;

private:
    friend class ForestPlacementStream;

    static constexpr uint32 kIndicesPerPage = 6;

    ForestPlacementRuntime(PageStreaming::PackedPlacementStore store, const ForestPlacementRuntimeDesc& desc);

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
        Mathematics::WorldPosition Min;
        Mathematics::WorldPosition Max;
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

    static void MergeBounds(Bounds& bounds, const Bounds& other);
    uint32 BuildNodes(uint32 first, uint32 count);
    void Visit(uint32 node, const ForestPlacementView& view, uint32 planeMask, double detailSquared,
               std::vector<CellRun>& visible);
    static void AppendVisible(uint32 first, uint32 count, std::vector<CellRun>& visible);
    void DelayRetry(uint32 page);
    bool CanRequestWithin(const ForestPlacementView& view, double distance) const;
    bool HasVisibleDetailPage(uint32 index, const ForestPlacementView& view, uint32 mask, double detailSquared) const;
    void GatherDemand(const ForestPlacementView& view);
    void ReleaseIfRetired(uint32 slot);
    // The oldest slot whose page is not wanted and whose last draw is complete, or SlotCount().
    uint32 FindReusableSlot() const;
    uint32 CountReusableSlots() const;
    void AppendReads(std::vector<ForestPageTicket>& reads);
    // The DetailWeight of a page that draws its trees for the demand view.
    float HandoffWeight(uint32 page) const;
    void EmitDraws(uint64 frame, ForestPlacementFrame& out);
    void EmitStand(uint32 first, uint32 end, std::vector<DrawSubCommand>& draws) const;

    PageStreaming::PackedPlacementStore m_Store;
    std::vector<Bounds> m_Leaves;
    std::vector<Node> m_Nodes;
    std::vector<Slot> m_Slots;
    PageStreaming::ResidentSlotPool<uint32> m_Residency;
    std::vector<CellRun> m_Visible;
    std::vector<CellRun> m_StandVisible;
    std::optional<ForestPlacementView> m_DemandView;
    std::optional<ForestPlacementView> m_StandView;
    std::optional<uint64> m_DemandRetryFrame;
    ForestDetailBudget m_DetailBudget;
    std::vector<uint32> m_Priority;
    std::vector<uint8> m_Wanted;
    std::vector<uint64> m_RetryAt;
    std::vector<uint32> m_FailedReads;
    std::vector<std::pair<double, uint32>> m_Candidates;
    std::vector<std::pair<uint32, uint32>> m_Ready;
    uint32 m_ReadsPerFrame;
    uint32 m_DetailCapacity;
    bool m_KeepCpuCopy = false;
    std::vector<PageStreaming::PackedPlacement> m_PlacementMirror;
    uint64 m_Frame = 0;
    uint64 m_ReadersDone = 0;
};

} // namespace GameEngine::Engine::Renderer
