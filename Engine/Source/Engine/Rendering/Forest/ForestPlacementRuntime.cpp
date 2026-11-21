#include "Engine/Rendering/Forest/ForestPlacementRuntime.h"

#include "Rendering/Common/Frustum.h"
#include "Rendering/Core/GPUDrawStreamBuilder.h"
#include "Platform/Environment.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>

namespace GameEngine::Engine::Renderer
{
namespace
{
constexpr uint32 kPlacementsPerSlot = PageStreaming::PackedPlacementStore::kPlacementsPerPage;
constexpr uint64 kRetryDelayFrames = 30;

Mathematics::Vector3 Relative(const std::array<double, 3>& position, const std::array<double, 3>& eye)
{
    return {static_cast<float>(position[0] - eye[0]), static_cast<float>(position[1] - eye[1]),
            static_cast<float>(position[2] - eye[2])};
}

double DistanceSquared(const std::array<double, 3>& min, const std::array<double, 3>& max,
                       const std::array<double, 3>& eye)
{
    double sum = 0;
    for (size_t axis = 0; axis < 3; ++axis)
    {
        const double delta = eye[axis] - std::clamp(eye[axis], min[axis], max[axis]);
        sum += delta * delta;
    }
    return sum;
}

std::optional<uint32> VisiblePlaneMask(const std::array<double, 3>& boundsMin,
                                      const std::array<double, 3>& boundsMax,
                                      const std::array<double, 3>& eye,
                                      const std::array<Mathematics::Vector4, 6>& planes, uint32 mask)
{
    if (mask == 0)
        return mask;
    const auto min = Relative(boundsMin, eye);
    const auto max = Relative(boundsMax, eye);
    for (uint32 i = 0; i < planes.size(); ++i)
    {
        const uint32 bit = 1u << i;
        if ((mask & bit) == 0)
            continue;
        const auto& p = planes[i];
        const float minX = p.x * (p.x >= 0.0f ? min.x : max.x);
        const float minY = p.y * (p.y >= 0.0f ? min.y : max.y);
        const float minZ = p.z * (p.z >= 0.0f ? min.z : max.z);
        const float maxX = p.x * (p.x >= 0.0f ? max.x : min.x);
        const float maxY = p.y * (p.y >= 0.0f ? max.y : min.y);
        const float maxZ = p.z * (p.z >= 0.0f ? max.z : min.z);
        // Keep borderline cases on the shared per-page frustum predicate.
        const float margin = 8 * std::numeric_limits<float>::epsilon() *
            (std::max(std::abs(minX), std::abs(maxX)) + std::max(std::abs(minY), std::abs(maxY)) +
             std::max(std::abs(minZ), std::abs(maxZ)) + std::abs(p.w));
        if (maxX + maxY + maxZ + p.w < -margin)
            return std::nullopt;
        if (minX + minY + minZ + p.w > margin)
            mask &= ~bit;
    }
    return mask;
}

bool SameStandView(const ForestPlacementView& a, const ForestPlacementView& b)
{
    return a.Eye == b.Eye && std::ranges::equal(a.EyeRelativePlanes, b.EyeRelativePlanes,
        [](const Mathematics::Vector4& lhs, const Mathematics::Vector4& rhs)
        {
            return lhs.x == rhs.x && lhs.y == rhs.y && lhs.z == rhs.z && lhs.w == rhs.w;
        });
}

uint64 RetryAfter(uint64 frame)
{
    return std::min(frame, std::numeric_limits<uint64>::max() - kRetryDelayFrames) + kRetryDelayFrames;
}

} // namespace

std::unique_ptr<ForestPlacementRuntime> ForestPlacementRuntime::Open(
    const std::filesystem::path& archive, std::span<const uint32> standIndexOffsets,
    uint32 slotCount, uint32 readsPerFrame, std::string& error)
{
    PageStreaming::PackedPlacementStore store;
    error = store.Open(archive);
    if (!error.empty())
        return {};
    if (standIndexOffsets.size() != store.Pages().size() + 1 ||
        std::adjacent_find(standIndexOffsets.begin(), standIndexOffsets.end(), std::greater_equal<uint32>{}) !=
            standIndexOffsets.end())
    {
        error = "Invalid stand index offsets for the placement pages";
        return {};
    }
    if (slotCount > std::numeric_limits<uint32>::max() / kPlacementsPerSlot)
    {
        error = "Forest detail slots exceed the indexed placement address range";
        return {};
    }
    slotCount = std::min(slotCount, static_cast<uint32>(store.Pages().size()));
    if (readsPerFrame == 0)
        slotCount = 0;
    return std::unique_ptr<ForestPlacementRuntime>(
        new ForestPlacementRuntime(std::move(store), standIndexOffsets, slotCount, readsPerFrame));
}

std::shared_ptr<ForestPlacementRuntime> ForestPlacementRuntime::OpenQuadStand(
    const std::filesystem::path& archive, std::string& error, uint32 slots, uint32 readsPerFrame, bool retainPlacements)
{
    PageStreaming::PackedPlacementStore store;
    error = store.Open(archive);
    if (!error.empty())
        return {};
    constexpr uint32 kIndicesPerQuad = 6;
    std::vector<uint32> offsets(store.Pages().size() + 1);
    for (uint32 i = 1; i < offsets.size(); ++i)
        offsets[i] = i * kIndicesPerQuad;
    slots = readsPerFrame == 0 ? 0 : std::min(slots, static_cast<uint32>(store.Pages().size()));
    if (slots > std::numeric_limits<uint32>::max() / kPlacementsPerSlot)
    {
        error = "Forest detail slots exceed the indexed placement address range";
        return {};
    }
    return std::shared_ptr<ForestPlacementRuntime>(
        new ForestPlacementRuntime(std::move(store), offsets, slots, readsPerFrame, retainPlacements));
}

ForestPlacementRuntime::ForestPlacementRuntime(PageStreaming::PackedPlacementStore store,
    std::span<const uint32> offsets, uint32 slots, uint32 readsPerFrame, bool retainPlacements)
    : m_Store(std::move(store)), m_IndexOffsets(offsets.begin(), offsets.end()), m_Slots(slots),
      m_Wanted(slots != 0 ? m_Store.Pages().size() : 0), m_RetryAt(m_Wanted.size()),
      m_ReadsPerFrame(std::min(slots, readsPerFrame)), m_RetainPlacements(retainPlacements)
{
    m_Leaves.reserve(m_Store.Pages().size());
    for (const auto& page : m_Store.Pages())
    {
        Bounds bounds{};
        for (size_t axis = 0; axis < 3; ++axis)
        {
            bounds.Min[axis] = page.Origin[axis] + page.LocalMin[axis];
            bounds.Max[axis] = page.Origin[axis] + page.LocalMax[axis];
        }
        m_Leaves.push_back(bounds);
    }
    BuildNodes(0, static_cast<uint32>(m_Leaves.size()));
    // ReleaseIfRetired already uses the completed draw fence. No additional frame-delay quarantine.
    m_Residency.Initialize(slots, 0);
    m_Ready.reserve(slots);
    m_Priority.reserve(slots);
}

void ForestPlacementRuntime::ResetResidency()
{
    std::ranges::fill(m_Slots, Slot{});
    std::ranges::fill(m_Wanted, uint8{0});
    std::ranges::fill(m_RetryAt, uint64{0});
    m_Residency.Reset();
    m_DemandView.reset();
    m_DemandRetryFrame.reset();
    m_Priority.clear();
    m_Ready.clear();
    m_Frame = 0;
    m_ReadersDone = 0;
}

bool ForestPlacementRuntime::CanRequestDetails(const ForestPlacementView& view) const
{
    if (m_Slots.empty() || !std::isfinite(view.DetailDistance) || view.DetailDistance <= 0)
        return false;
    const double distance = view.DetailDistance;
    return HasVisibleDetailPage(0, view, {view.Eye.X, view.Eye.Y, view.Eye.Z}, 0x3fu, distance * distance);
}

bool ForestPlacementRuntime::CanPrefetchAtlas(const ForestPlacementView& view) const
{
    // Allow the default 64-tile atlas to start baking before the first page is needed.
    constexpr float kAtlasPrefetchDistanceScale = 1.4f;
    ForestPlacementView lookahead = view;
    lookahead.DetailDistance *= kAtlasPrefetchDistanceScale;
    return CanRequestDetails(lookahead);
}

bool ForestPlacementRuntime::HasVisibleDetailPage(uint32 index, const ForestPlacementView& view,
                                                   const std::array<double, 3>& eye, uint32 mask,
                                                   double detailSquared) const
{
    const Node& node = m_Nodes[index];
    if (DistanceSquared(node.Box.Min, node.Box.Max, eye) >= detailSquared)
        return false;
    const auto activePlanes = VisiblePlaneMask(node.Box.Min, node.Box.Max, eye, view.EyeRelativePlanes, mask);
    if (!activePlanes)
        return false;
    if (node.Right != 0)
        return HasVisibleDetailPage(index + 1, view, eye, *activePlanes, detailSquared) ||
               HasVisibleDetailPage(node.Right, view, eye, *activePlanes, detailSquared);
    for (uint32 page = node.First; page < node.First + node.Count; ++page)
    {
        const Bounds& box = m_Leaves[page];
        if (DistanceSquared(box.Min, box.Max, eye) < detailSquared &&
            (*activePlanes == 0 ||
             Rendering::TestAabbFrustum(Relative(box.Min, eye), Relative(box.Max, eye), view.EyeRelativePlanes.data())))
            return true;
    }
    return false;
}

uint32 ForestPlacementRuntime::BuildNodes(uint32 first, uint32 count)
{
    constexpr uint32 kLeafPages = 8;
    const auto mergeBounds = [](Bounds& bounds, const Bounds& other)
    {
        for (size_t axis = 0; axis < 3; ++axis)
        {
            bounds.Min[axis] = std::min(bounds.Min[axis], other.Min[axis]);
            bounds.Max[axis] = std::max(bounds.Max[axis], other.Max[axis]);
        }
    };

    const uint32 node = static_cast<uint32>(m_Nodes.size());
    m_Nodes.push_back({m_Leaves[first], first, count});
    if (count <= kLeafPages)
    {
        for (uint32 i = first + 1; i < first + count; ++i)
            mergeBounds(m_Nodes[node].Box, m_Leaves[i]);
    }
    else
    {
        const uint32 leftCount = count / 2;
        const uint32 left = BuildNodes(first, leftCount);
        const uint32 right = BuildNodes(first + leftCount, count - leftCount);
        m_Nodes[node].Box = m_Nodes[left].Box;
        mergeBounds(m_Nodes[node].Box, m_Nodes[right].Box);
        m_Nodes[node].Right = right;
    }
    return node;
}

void ForestPlacementRuntime::AppendVisible(uint32 first, uint32 count, std::vector<CellRun>& visible)
{
    if (!visible.empty() && visible.back().First + visible.back().Count == first)
        visible.back().Count += count;
    else
        visible.push_back({first, count});
}

void ForestPlacementRuntime::Visit(uint32 index, const ForestPlacementView& view, const std::array<double, 3>& eye,
                                   uint32 mask, double detailSquared, std::vector<CellRun>& visible)
{
    const Node& node = m_Nodes[index];
    const auto activePlanes = VisiblePlaneMask(node.Box.Min, node.Box.Max, eye, view.EyeRelativePlanes, mask);
    if (!activePlanes)
        return;
    mask = *activePlanes;

    if (detailSquared > 0.0 && DistanceSquared(node.Box.Min, node.Box.Max, eye) >= detailSquared)
        detailSquared = 0;
    if (mask == 0 && detailSquared == 0)
    {
        AppendVisible(node.First, node.Count, visible);
        return;
    }
    if (node.Right != 0)
    {
        Visit(index + 1, view, eye, mask, detailSquared, visible);
        Visit(node.Right, view, eye, mask, detailSquared, visible);
        return;
    }
    for (uint32 page = node.First; page < node.First + node.Count; ++page)
    {
        const Bounds& box = m_Leaves[page];
        if (mask != 0 && !Rendering::TestAabbFrustum(Relative(box.Min, eye), Relative(box.Max, eye),
                                                    view.EyeRelativePlanes.data()))
            continue;
        AppendVisible(page, 1, visible);
        if (detailSquared > 0)
        {
            const double distance = DistanceSquared(box.Min, box.Max, eye);
            if (distance >= detailSquared)
                continue;
            const uint64 retry = m_RetryAt[page];
            if (m_Frame >= retry)
                m_Candidates.emplace_back(distance, page);
            else if (!m_DemandRetryFrame || retry < *m_DemandRetryFrame)
                m_DemandRetryFrame = retry;
        }
    }
}

void ForestPlacementRuntime::GatherStand(const ForestPlacementView& view, std::vector<DrawSubCommand>& draws)
{
    draws.clear();
    if (!m_StandView || !SameStandView(*m_StandView, view))
    {
        m_StandView = view;
        m_StandVisible.clear();
        Visit(0, view, {view.Eye.X, view.Eye.Y, view.Eye.Z}, 0x3fu, 0, m_StandVisible);
    }
    for (const CellRun& run : m_StandVisible)
        EmitStand(run.First, run.First + run.Count, draws);
}

void ForestPlacementRuntime::GatherDemand(const ForestPlacementView& view)
{
    if (m_DemandView && m_DemandView->DetailDistance == view.DetailDistance &&
        SameStandView(*m_DemandView, view) && (!m_DemandRetryFrame || m_Frame < *m_DemandRetryFrame))
        return;
    m_DemandView = view;
    m_DemandRetryFrame.reset();
    for (uint32 page : m_Priority)
        m_Wanted[page] = 0;
    m_Priority.clear();
    m_Visible.clear();
    m_Candidates.clear();
    const double detail = m_Slots.empty() || !std::isfinite(view.DetailDistance) ?
        0.0 : std::max(0.0f, view.DetailDistance);
    Visit(0, view, {view.Eye.X, view.Eye.Y, view.Eye.Z}, 0x3fu, detail * detail, m_Visible);

    const size_t count = std::min(m_Candidates.size(), m_Slots.size());
    std::partial_sort(m_Candidates.begin(), m_Candidates.begin() + count, m_Candidates.end());
    for (const auto& [distance, page] : std::span(m_Candidates).first(count))
    {
        m_Wanted[page] = 1;
        m_Priority.push_back(page);
    }
}

void ForestPlacementRuntime::ReleaseIfRetired(uint32 slotIndex)
{
    Slot& slot = m_Slots[slotIndex];
    if (slot.Current != Stage::Ready || !slot.Retiring || slot.LastReader > m_ReadersDone)
        return;
    const bool released = m_Residency.Release(slot.Ticket.Page, slot.Ticket.Generation);
    assert(released);
    (void)released;
    slot = {};
}

bool ForestPlacementRuntime::Prepare(const ForestPlacementView& view, uint64 frame, uint64 completedReaders,
                                     ForestPlacementFrame& out)
{
    out.StandDraws.clear();
    out.DetailedDraws.clear();
    out.StandFades.clear();
    out.Reads.clear();
    out.CancelReads.clear();
    out.VisibleCells = 0;
    out.WantsDetails = false;
    if (frame <= m_Frame || completedReaders < m_ReadersDone || completedReaders >= frame)
        return false;

    m_Frame = frame;
    m_ReadersDone = completedReaders;
    GatherDemand(view);
    out.WantsDetails = !m_Priority.empty();
    m_Residency.BeginFrame(frame);
    for (uint32 i = 0; i < m_Slots.size(); ++i)
    {
        Slot& slot = m_Slots[i];
        if (slot.Current == Stage::Empty)
            continue;
        const bool wanted = m_Wanted[slot.Ticket.Page] != 0;
        if (wanted && slot.Current == Stage::Ready && slot.LastReader != 0)
        {
            // The validated slot is still ours. A returning view can read it again
            // and EmitDraws will extend its fence before it can be reused.
            slot.Retiring = false;
        }
        else if (!wanted && !slot.Retiring)
        {
            slot.Retiring = true;
            if (slot.Current == Stage::Reading)
            {
                const auto cancelled = slot.Ticket;
                out.CancelReads.push_back(cancelled);
                // Reads have no GPU consumer yet. Reclaim the slot before selecting new pages.
                CompleteRead(cancelled, {});
            }
        }
        if (slot.LastReader == 0)
            ReleaseIfRetired(i);
    }
    AppendReads(out.Reads);
    EmitDraws(view, frame, out);
    return true;
}

void ForestPlacementRuntime::AppendReads(std::vector<ForestPageTicket>& reads)
{
    for (uint32 page : m_Priority)
    {
        if (reads.size() >= m_ReadsPerFrame)
            break;
        // A read reported corrupt bytes after this frame's priorities were built.
        if (m_Frame < m_RetryAt[page] || m_Residency.Find(page).IsResident())
            continue;
        if (m_Residency.FreeCount() == 0)
        {
            // Keep unused GPU pages warm until a visible page needs the slot.
            // Pending readers and wanted pages cannot be displaced.
            uint32 victim = static_cast<uint32>(m_Slots.size());
            uint64 oldest = std::numeric_limits<uint64>::max();
            for (uint32 i = 0; i < m_Slots.size(); ++i)
            {
                const Slot& slot = m_Slots[i];
                if (slot.Current == Stage::Ready && slot.Retiring &&
                    slot.LastReader <= m_ReadersDone && slot.LastReader < oldest)
                {
                    victim = i;
                    oldest = slot.LastReader;
                }
            }
            if (victim == m_Slots.size())
                break;
            ReleaseIfRetired(victim);
        }
        const auto ref = m_Residency.Acquire(page, 0.0f);
        assert(ref.IsResident() && m_Slots[ref.Slot].Current == Stage::Empty);
        const ForestPageTicket ticket{ref.Slot, page, ref.Generation};
        m_Slots[ref.Slot] = Slot{ticket, Stage::Reading};
        reads.push_back(ticket);
    }
}

void ForestPlacementRuntime::DelayRetry(uint32 page)
{
    m_RetryAt[page] = RetryAfter(m_Frame);
    m_DemandView.reset();
}

bool ForestPlacementRuntime::CompleteRead(ForestPageTicket ticket, std::span<const uint8> bytes)
{
    if (ticket.Slot >= m_Slots.size())
        return false;
    Slot& slot = m_Slots[ticket.Slot];
    if (slot.Ticket != ticket || slot.Current != Stage::Reading)
        return false;
    if (slot.Retiring || !m_Store.VerifyPage(ticket.Page, bytes))
    {
        if (!slot.Retiring)
            DelayRetry(ticket.Page);
        slot.Retiring = true;
        slot.Current = Stage::Ready;
        ReleaseIfRetired(ticket.Slot);
        return false;
    }
    constexpr uint16 maxCoord = std::numeric_limits<uint16>::max();
    ForestPlacementRootBounds roots{maxCoord, maxCoord, 0, 0};
    for (size_t offset = 0; offset < bytes.size(); offset += sizeof(PageStreaming::PackedPlacement))
    {
        PageStreaming::PackedPlacement packed;
        std::memcpy(&packed, bytes.data() + offset, sizeof(packed));
        roots.MinX = std::min(roots.MinX, packed.X);
        roots.MinZ = std::min(roots.MinZ, packed.Z);
        roots.MaxX = std::max(roots.MaxX, packed.X);
        roots.MaxZ = std::max(roots.MaxZ, packed.Z);
    }
    slot.RootBounds = roots;
    if (m_RetainPlacements)
    {
        if (m_PlacementMirror.empty())
            m_PlacementMirror.resize(static_cast<size_t>(m_Slots.size()) * kPlacementsPerSlot);
        auto* destination = m_PlacementMirror.data() + static_cast<size_t>(ticket.Slot) * kPlacementsPerSlot;
        std::memcpy(destination, bytes.data(), bytes.size());
    }
    slot.Current = Stage::Uploading;
    return true;
}

std::span<const PageStreaming::PackedPlacement> ForestPlacementRuntime::ReadyPlacements(ForestPageTicket ticket) const
{
    if (m_PlacementMirror.empty() || ticket.Slot >= m_Slots.size())
        return {};
    const auto& slot = m_Slots[ticket.Slot];
    if (slot.Ticket != ticket || slot.Current != Stage::Ready || slot.Retiring)
        return {};
    return std::span(m_PlacementMirror).subspan(static_cast<size_t>(ticket.Slot) * kPlacementsPerSlot,
                                               m_Store.Pages()[ticket.Page].Count);
}

std::optional<ForestPlacementRootBounds> ForestPlacementRuntime::ReadyRootBounds(ForestPageTicket ticket) const
{
    if (ticket.Slot >= m_Slots.size())
        return std::nullopt;
    const auto& slot = m_Slots[ticket.Slot];
    if (slot.Ticket != ticket || slot.Current != Stage::Ready || slot.Retiring)
        return std::nullopt;
    return slot.RootBounds;
}

bool ForestPlacementRuntime::CanRecordUpload(ForestPageTicket ticket) const
{
    if (ticket.Slot >= m_Slots.size())
        return false;
    const Slot& slot = m_Slots[ticket.Slot];
    return slot.Ticket == ticket && slot.Current == Stage::Uploading && !slot.Retiring;
}

bool ForestPlacementRuntime::CompleteUpload(ForestPageTicket ticket, bool succeeded)
{
    if (ticket.Slot >= m_Slots.size())
        return false;
    Slot& slot = m_Slots[ticket.Slot];
    if (slot.Ticket != ticket || slot.Current != Stage::Uploading)
        return false;
    slot.Current = Stage::Ready;
    if (!slot.Retiring && !succeeded)
    {
        DelayRetry(ticket.Page);
        slot.Retiring = true;
    }
    if (!slot.Retiring)
        return true;
    ReleaseIfRetired(ticket.Slot);
    return false;
}

void ForestPlacementRuntime::EmitStand(uint32 first, uint32 end, std::vector<DrawSubCommand>& draws) const
{
    if (first == end)
        return;
    const uint32 offset = m_IndexOffsets[first];
    draws.push_back({.IndexCount = m_IndexOffsets[end] - offset, .FirstIndex = offset});
}

void ForestPlacementRuntime::EmitDraws(const ForestPlacementView& view, uint64 frame, ForestPlacementFrame& out)
{
    m_Ready.clear();
    for (uint32 i = 0; i < m_Slots.size(); ++i)
    {
        Slot& slot = m_Slots[i];
        if (slot.Current == Stage::Ready && !slot.Retiring)
        {
            slot.LastReader = frame;
            m_Ready.emplace_back(slot.Ticket.Page, i);
        }
    }
    std::ranges::sort(m_Ready);
    auto ready = m_Ready.begin();
    for (const auto& run : m_Visible)
    {
        const uint32 end = run.First + run.Count;
        uint32 cursor = run.First;
        out.VisibleCells += run.Count;
        while (ready != m_Ready.end() && ready->first < end)
        {
            const auto [page, slot] = *ready++;
            assert(cursor <= page);
            uint32 fadeCode = 0;
            if (view.DetailFadeWidth > 0.0f)
            {
                const auto& bounds = m_Leaves[page];
                const double distance = std::sqrt(DistanceSquared(bounds.Min, bounds.Max,
                    {view.Eye.X, view.Eye.Y, view.Eye.Z}));
                const double weight = std::clamp((view.DetailDistance - distance) / view.DetailFadeWidth, 0.0, 1.0);
                static const bool useGradientMask =
                    Platform::EnvironmentSwitchEnabled("GE_FOREST_SPLAT_GRADIENT_MASK", true);
                using Fade = Rendering::GPUDrawStreamBuilder;
                constexpr uint32 kGradientMask = 0x00800000u;
                const uint32 levels = useGradientMask ? Fade::kLodFadeMaxWeight : 16u;
                const uint32 step = static_cast<uint32>(std::round(weight * levels));
                if (step == 0)
                    continue;
                if (step < levels)
                {
                    // The Bayer control has 16 steps. Store an exact threshold between its samples.
                    const uint32 quantized = useGradientMask ? step
                        : ((step - 1u) * Fade::kLodFadeMaxWeight) / 16u + 1u;
                    fadeCode = (quantized << Fade::kLodFadeWeightShift) |
                        (useGradientMask ? kGradientMask : 0u);
                    const uint32 first = m_IndexOffsets[page];
                    out.StandFades.push_back({{.IndexCount = m_IndexOffsets[page + 1] - first,
                                              .FirstIndex = first}, fadeCode | Fade::kLodFadePhaseBit});
                }
            }
            EmitStand(cursor, page, out.StandDraws);
            out.DetailedDraws.push_back({m_Slots[slot].Ticket, slot * kPlacementsPerSlot,
                                        m_Store.Pages()[page].Count, fadeCode});
            cursor = page + 1;
        }
        EmitStand(cursor, end, out.StandDraws);
    }
    assert(ready == m_Ready.end());
}

} // namespace GameEngine::Engine::Renderer
