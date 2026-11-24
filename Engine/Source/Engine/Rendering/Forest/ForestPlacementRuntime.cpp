#include "ForestPlacementRuntime.h"

#include "PageStreaming/PageLoader.h"
#include "Rendering/Common/Frustum.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <ranges>
#include <tuple>

namespace GameEngine::Engine::Renderer
{
namespace
{
constexpr uint32 kPlacementsPerSlot = PageStreaming::PackedPlacementStore::kPlacementsPerPage;
// All six frustum planes still to test.
constexpr uint32 kAllPlanes = 0x3fu;
constexpr float kDetailPrefetchDistanceScale = 1.4f;
constexpr uint8 kPrefetchDetail = 1;
constexpr uint8 kDrawHandoff = 2;
constexpr uint8 kDrawDetail = 3;
constexpr uint8 kAllCascades = (1u << kMaxDepthEmitCascades) - 1u;
// The float rounding margin of a plane test, in epsilons of the summed magnitudes.
constexpr float kPlaneMarginEpsilons = 8.0f;

double AxisDistance(double eye, double min, double max)
{
    return eye - std::clamp(eye, min, max);
}

double DistanceSquared(const Mathematics::WorldPosition& min, const Mathematics::WorldPosition& max,
                       const Mathematics::WorldPosition& eye)
{
    const double x = AxisDistance(eye.X, min.X, max.X);
    const double y = AxisDistance(eye.Y, min.Y, max.Y);
    const double z = AxisDistance(eye.Z, min.Z, max.Z);
    return x * x + y * y + z * z;
}

std::optional<uint32> VisiblePlaneMask(const Mathematics::WorldPosition& boundsMin,
                                      const Mathematics::WorldPosition& boundsMax,
                                      const Mathematics::WorldPosition& eye,
                                      const std::array<Mathematics::Vector4, 6>& planes, uint32 mask)
{
    if (mask == 0)
        return mask;
    const Mathematics::Vector3 min = boundsMin - eye;
    const Mathematics::Vector3 max = boundsMax - eye;
    for (uint32 planeIndex = 0; planeIndex < planes.size(); ++planeIndex)
    {
        const uint32 bit = 1u << planeIndex;
        if ((mask & bit) == 0)
            continue;
        const Mathematics::Vector4& plane = planes[planeIndex];
        const float minX = plane.x * (plane.x >= 0.0f ? min.x : max.x);
        const float minY = plane.y * (plane.y >= 0.0f ? min.y : max.y);
        const float minZ = plane.z * (plane.z >= 0.0f ? min.z : max.z);
        const float maxX = plane.x * (plane.x >= 0.0f ? max.x : min.x);
        const float maxY = plane.y * (plane.y >= 0.0f ? max.y : min.y);
        const float maxZ = plane.z * (plane.z >= 0.0f ? max.z : min.z);
        // Keep borderline cases on the shared per-page frustum predicate.
        const float margin = kPlaneMarginEpsilons * std::numeric_limits<float>::epsilon() *
            (std::max(std::abs(minX), std::abs(maxX)) + std::max(std::abs(minY), std::abs(maxY)) +
             std::max(std::abs(minZ), std::abs(maxZ)) + std::abs(plane.w));
        if (maxX + maxY + maxZ + plane.w < -margin)
            return std::nullopt;
        if (minX + minY + minZ + plane.w > margin)
            mask &= ~bit;
    }
    return mask;
}

// Meters from the eye within which pages draw their trees: the detail distance and the handoff band.
double DetailReach(const ForestPlacementView& view)
{
    if (!std::isfinite(view.DetailDistance) || view.DetailDistance <= 0.0f)
        return 0.0;
    const float handoff = std::isfinite(view.HandoffWidth) ? std::max(0.0f, view.HandoffWidth) : 0.0f;
    return static_cast<double>(view.DetailDistance) + handoff;
}

bool SameStandView(const ForestPlacementView& a, const ForestPlacementView& b)
{
    return a.Eye == b.Eye && std::ranges::equal(a.EyeRelativePlanes, b.EyeRelativePlanes,
        [](const Mathematics::Vector4& lhs, const Mathematics::Vector4& rhs)
        {
            return lhs.x == rhs.x && lhs.y == rhs.y && lhs.z == rhs.z && lhs.w == rhs.w;
        });
}

// Slots that sun cascades add to the detail and transition slots. An archive with fewer pages
// gets fewer: then every page can hold a slot at the same time.
uint32 PhysicalShadowSlots(const ForestPlacementRuntimeDesc& desc, uint32 pages)
{
    return std::min(desc.ShadowSlots, pages - desc.DetailSlots - desc.TransitionSlots);
}

} // namespace

std::shared_ptr<ForestPlacementRuntime> ForestPlacementRuntime::Open(const std::filesystem::path& archive,
                                                                     const ForestPlacementRuntimeDesc& desc,
                                                                     std::string& error)
{
    PageStreaming::PackedPlacementStore store;
    error = store.Open(archive);
    if (!error.empty())
        return {};
    const auto pages = static_cast<uint32>(store.Pages().size());
    ForestPlacementRuntimeDesc clamped = desc;
    clamped.DetailSlots = desc.ReadsPerFrame == 0 ? 0 : std::min(desc.DetailSlots, pages);
    clamped.TransitionSlots = clamped.DetailSlots == 0 ? 0 : std::min(desc.TransitionSlots, pages - clamped.DetailSlots);
    clamped.ShadowSlots = clamped.DetailSlots == 0 ? 0 : std::min(desc.ShadowSlots, pages);
    if (uint64{clamped.DetailSlots} + clamped.TransitionSlots + PhysicalShadowSlots(clamped, pages) >
        std::numeric_limits<uint32>::max() / kPlacementsPerSlot)
    {
        error = "The forest detail slots exceed the placement address range. Use fewer detail slots.";
        return {};
    }
    return std::shared_ptr<ForestPlacementRuntime>(new ForestPlacementRuntime(std::move(store), clamped));
}

ForestPlacementRuntime::ForestPlacementRuntime(PageStreaming::PackedPlacementStore store,
                                               const ForestPlacementRuntimeDesc& desc)
    : m_Store(std::move(store)),
      m_Slots(desc.DetailSlots + desc.TransitionSlots +
              PhysicalShadowSlots(desc, static_cast<uint32>(m_Store.Pages().size()))),
      m_Wanted(desc.DetailSlots != 0 ? m_Store.Pages().size() : 0), m_ShadowMasks(m_Wanted.size()),
      m_RetryAt(m_Wanted.size()), m_FailedReads(m_Wanted.size()),
      m_ReadsPerFrame(std::min(desc.DetailSlots, desc.ReadsPerFrame)), m_DetailCapacity(desc.DetailSlots),
      m_TransitionCapacity(desc.TransitionSlots), m_ShadowCapacity(desc.ShadowSlots), m_KeepCpuCopy(desc.KeepCpuCopy)
{
    m_Leaves.reserve(m_Store.Pages().size());
    for (const auto& page : m_Store.Pages())
        m_Leaves.push_back({page.Origin + page.LocalMin, page.Origin + page.LocalMax});
    BuildNodes(0, static_cast<uint32>(m_Leaves.size()));
    // The reader fence guards slot reuse, so the pool has no quarantine.
    m_Residency.Initialize(static_cast<uint32>(m_Slots.size()), 0);
    m_Ready.reserve(m_Slots.size());
    m_Priority.reserve(m_Slots.size());
}

void ForestPlacementRuntime::ResetResidency()
{
    std::ranges::fill(m_Slots, Slot{});
    std::ranges::fill(m_Wanted, uint8{0});
    std::ranges::fill(m_ShadowMasks, uint8{0});
    std::ranges::fill(m_RetryAt, uint64{0});
    std::ranges::fill(m_FailedReads, uint32{0});
    m_Residency.Reset();
    m_DemandView.reset();
    m_DemandRetryFrame.reset();
    m_Priority.clear();
    m_ShadowPages.clear();
    m_ReadOrder.clear();
    m_Ready.clear();
    m_Frame = 0;
    m_ReadersDone = 0;
}

bool ForestPlacementRuntime::CanPrefetchDetails(const ForestPlacementView& view) const
{
    return CanRequestWithin(view, DetailReach(view) * kDetailPrefetchDistanceScale);
}

bool ForestPlacementRuntime::CanRequestWithin(const ForestPlacementView& view, double distance) const
{
    return !m_Slots.empty() && std::isfinite(distance) && distance > 0.0 &&
           HasVisibleDetailPage(0, view, kAllPlanes, distance * distance);
}

bool ForestPlacementRuntime::HasVisibleDetailPage(uint32 index, const ForestPlacementView& view, uint32 mask,
                                                   double detailSquared) const
{
    const Node& node = m_Nodes[index];
    if (DistanceSquared(node.Box.Min, node.Box.Max, view.Eye) >= detailSquared)
        return false;
    const auto activePlanes = VisiblePlaneMask(node.Box.Min, node.Box.Max, view.Eye, view.EyeRelativePlanes, mask);
    if (!activePlanes)
        return false;
    if (node.Right != 0)
        return HasVisibleDetailPage(index + 1, view, *activePlanes, detailSquared) ||
               HasVisibleDetailPage(node.Right, view, *activePlanes, detailSquared);
    for (uint32 page = node.First; page < node.First + node.Count; ++page)
    {
        const Bounds& box = m_Leaves[page];
        if (DistanceSquared(box.Min, box.Max, view.Eye) < detailSquared &&
            (*activePlanes == 0 ||
             Rendering::TestAabbFrustum(box.Min - view.Eye, box.Max - view.Eye, view.EyeRelativePlanes.data())))
            return true;
    }
    return false;
}

void ForestPlacementRuntime::MergeBounds(Bounds& bounds, const Bounds& other)
{
    bounds.Min = {std::min(bounds.Min.X, other.Min.X), std::min(bounds.Min.Y, other.Min.Y),
                  std::min(bounds.Min.Z, other.Min.Z)};
    bounds.Max = {std::max(bounds.Max.X, other.Max.X), std::max(bounds.Max.Y, other.Max.Y),
                  std::max(bounds.Max.Z, other.Max.Z)};
}

uint32 ForestPlacementRuntime::BuildNodes(uint32 first, uint32 count)
{
    constexpr uint32 kLeafPages = 8;
    const uint32 node = static_cast<uint32>(m_Nodes.size());
    m_Nodes.push_back({m_Leaves[first], first, count});
    if (count <= kLeafPages)
    {
        for (uint32 leaf = first + 1; leaf < first + count; ++leaf)
            MergeBounds(m_Nodes[node].Box, m_Leaves[leaf]);
    }
    else
    {
        const uint32 leftCount = count / 2;
        const uint32 left = BuildNodes(first, leftCount);
        const uint32 right = BuildNodes(first + leftCount, count - leftCount);
        m_Nodes[node].Box = m_Nodes[left].Box;
        MergeBounds(m_Nodes[node].Box, m_Nodes[right].Box);
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

void ForestPlacementRuntime::Visit(uint32 index, const ForestPlacementView& view, uint32 mask, double detailSquared,
                                   std::vector<CellRun>& visible)
{
    const Node& node = m_Nodes[index];
    const auto activePlanes = VisiblePlaneMask(node.Box.Min, node.Box.Max, view.Eye, view.EyeRelativePlanes, mask);
    if (!activePlanes)
        return;
    mask = *activePlanes;

    if (detailSquared > 0.0 && DistanceSquared(node.Box.Min, node.Box.Max, view.Eye) >= detailSquared)
        detailSquared = 0;
    if (mask == 0 && detailSquared == 0)
    {
        AppendVisible(node.First, node.Count, visible);
        return;
    }
    if (node.Right != 0)
    {
        Visit(index + 1, view, mask, detailSquared, visible);
        Visit(node.Right, view, mask, detailSquared, visible);
        return;
    }
    for (uint32 page = node.First; page < node.First + node.Count; ++page)
    {
        const Bounds& box = m_Leaves[page];
        if (mask != 0 && !Rendering::TestAabbFrustum(box.Min - view.Eye, box.Max - view.Eye,
                                                    view.EyeRelativePlanes.data()))
            continue;
        AppendVisible(page, 1, visible);
        if (detailSquared > 0)
        {
            const double distance = DistanceSquared(box.Min, box.Max, view.Eye);
            if (distance >= detailSquared)
                continue;
            if (distance < static_cast<double>(view.DetailDistance) * view.DetailDistance)
                ++m_DetailBudget.Eligible;
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
        Visit(0, view, kAllPlanes, 0, m_StandVisible);
    }
    for (const CellRun& run : m_StandVisible)
        EmitStand(run.First, run.First + run.Count, draws);
}

std::span<const ForestShadowPage> ForestPlacementRuntime::GatherShadowPages(
    std::span<const ForestPlacementView> cascades, const Mathematics::WorldPosition& eye, float detailDistance)
{
    m_ShadowQuery.clear();
    if (m_Slots.empty() || !std::isfinite(detailDistance) || detailDistance <= 0.0f)
        return {};
    const double detailSquared = static_cast<double>(detailDistance) * detailDistance;
    m_ShadowCandidates.clear();
    const auto count = static_cast<uint32>(std::min(cascades.size(), m_ShadowViews.size()));
    for (uint32 cascade = 0; cascade < count; ++cascade)
    {
        const ForestPlacementView& view = cascades[cascade];
        std::vector<CellRun>& visible = m_ShadowVisible[cascade];
        if (!m_ShadowViews[cascade] || !SameStandView(*m_ShadowViews[cascade], view))
        {
            m_ShadowViews[cascade] = view;
            visible.clear();
            Visit(0, view, kAllPlanes, 0, visible);
        }
        const auto mask = static_cast<uint8>(1u << cascade);
        for (const CellRun& run : visible)
        {
            for (uint32 page = run.First; page < run.First + run.Count; ++page)
            {
                const double distance = DistanceSquared(m_Leaves[page].Min, m_Leaves[page].Max, eye);
                if (distance < detailSquared)
                    m_ShadowCandidates.push_back({distance, cascade, page, mask});
            }
        }
    }
    // One entry per page: the first cascade that wants it, with the bits of every cascade.
    std::ranges::sort(m_ShadowCandidates, {}, [](const ShadowCandidate& candidate)
                      { return std::pair(candidate.Page, candidate.Cascade); });
    size_t unique = 0;
    for (const ShadowCandidate& candidate : m_ShadowCandidates)
    {
        if (unique != 0 && m_ShadowCandidates[unique - 1].Page == candidate.Page)
            m_ShadowCandidates[unique - 1].CascadeMask |= candidate.CascadeMask;
        else
            m_ShadowCandidates[unique++] = candidate;
    }
    m_ShadowCandidates.resize(unique);
    std::ranges::sort(m_ShadowCandidates, {}, [](const ShadowCandidate& candidate)
                      { return std::tuple(candidate.Cascade, candidate.DistanceSquared, candidate.Page); });
    for (const ShadowCandidate& candidate : m_ShadowCandidates)
        m_ShadowQuery.push_back({candidate.Page, candidate.CascadeMask});
    return m_ShadowQuery;
}

void ForestPlacementRuntime::GatherDemand(const ForestPlacementView& view)
{
    if (m_DemandView && m_DemandView->DetailDistance == view.DetailDistance &&
        m_DemandView->HandoffWidth == view.HandoffWidth && SameStandView(*m_DemandView, view) &&
        (!m_DemandRetryFrame || m_Frame < *m_DemandRetryFrame))
        return;
    m_DemandView = view;
    m_DemandRetryFrame.reset();
    for (uint32 page : m_Priority)
        m_Wanted[page] = 0;
    m_Priority.clear();
    m_Visible.clear();
    m_Candidates.clear();
    m_DetailBudget = {};
    const double detail = m_Slots.empty() ? 0.0 : DetailReach(view);
    const double distanceSquared = detail * detail;
    const double authoredSquared = static_cast<double>(view.DetailDistance) * view.DetailDistance;
    const double prefetchDistance = detail * kDetailPrefetchDistanceScale;
    Visit(0, view, kAllPlanes,
          prefetchDistance * prefetchDistance, m_Visible);

    const size_t count = std::min(m_Candidates.size(), static_cast<size_t>(m_DetailCapacity));
    // One extra ordered candidate identifies the closest budget omission without scanning the remaining pages.
    const size_t sorted = count < m_Candidates.size() ? count + 1 : count;
    std::partial_sort(m_Candidates.begin(), m_Candidates.begin() + sorted, m_Candidates.end());
    if (count < sorted && m_Candidates[count].first < authoredSquared)
        m_DetailBudget.NearestOmitted = std::sqrt(m_Candidates[count].first);
    // Distance order puts every draw request before speculative reads.
    uint32 prefetchCount = 0;
    for (const auto& [distance, page] : std::span(m_Candidates).first(count))
    {
        const bool prefetch = distance >= distanceSquared;
        if (prefetch && prefetchCount == m_ReadsPerFrame)
            break;
        const bool withinDetailDistance = distance < authoredSquared;
        m_Wanted[page] = prefetch ? kPrefetchDetail : (withinDetailDistance ? kDrawDetail : kDrawHandoff);
        m_Priority.push_back(page);
        if (prefetch)
            ++prefetchCount;
        else if (withinDetailDistance)
            ++m_DetailBudget.Selected;
    }
}

void ForestPlacementRuntime::GatherShadowDemand(std::span<const ForestShadowPage> pages)
{
    for (uint32 page : m_ShadowPages)
        m_ShadowMasks[page] = 0;
    m_ShadowPages.clear();
    // Pages that the view draws do not count against the cap: they hold their slot for color.
    uint32 shadowOnly = 0;
    for (const auto& [page, mask] : pages)
    {
        const auto cascades = static_cast<uint8>(mask & kAllCascades);
        // A page that waits for a read retry does not hold a place under the cap.
        if (page >= m_ShadowMasks.size() || cascades == 0 || m_Frame < m_RetryAt[page])
            continue;
        if (m_ShadowMasks[page] != 0)
        {
            m_ShadowMasks[page] |= cascades;
            continue;
        }
        if (m_Wanted[page] < kDrawHandoff)
        {
            if (shadowOnly == m_ShadowCapacity)
                continue;
            ++shadowOnly;
        }
        m_ShadowMasks[page] = cascades;
        m_ShadowPages.push_back(page);
    }
}

bool ForestPlacementRuntime::IsShadowOnly(uint32 page) const
{
    return m_ShadowMasks[page] != 0 && m_Wanted[page] < kDrawHandoff;
}

void ForestPlacementRuntime::BuildReadOrder()
{
    // Color draw demand, then shadow demand, then color prefetch.
    const auto firstPrefetch =
        std::ranges::find_if(m_Priority, [this](uint32 page) { return m_Wanted[page] == kPrefetchDetail; });
    m_ReadOrder.assign(m_Priority.begin(), firstPrefetch);
    for (uint32 page : m_ShadowPages)
    {
        if (m_Wanted[page] < kDrawHandoff)
            m_ReadOrder.push_back(page);
    }
    for (uint32 page : std::ranges::subrange(firstPrefetch, m_Priority.end()))
    {
        if (m_ShadowMasks[page] == 0)
            m_ReadOrder.push_back(page);
    }
}

void ForestPlacementRuntime::ReleaseIfRetired(uint32 slotIndex)
{
    Slot& slot = m_Slots[slotIndex];
    if (slot.Current != Stage::Ready || !slot.Retiring)
        return;
    // Every caller passes a slot that was never drawn or whose last draw is complete.
    assert(slot.LastReader <= m_ReadersDone);
    const bool released = m_Residency.Release(slot.Ticket.Page, slot.Ticket.Generation);
    assert(released);
    (void)released;
    slot = {};
}

bool ForestPlacementRuntime::Prepare(const ForestPlacementView& view, uint64 frame, uint64 completedReaders,
                                     std::span<const ForestShadowPage> shadowPages, ForestPlacementFrame& out)
{
    out.StandDraws.clear();
    out.DetailedDraws.clear();
    out.ShadowDraws.clear();
    out.StandFades.clear();
    out.Reads.clear();
    out.CancelReads.clear();
    out.DetailBudget = {};
    out.VisiblePages = 0;
    out.WantsDetails = false;
    if (frame <= m_Frame || completedReaders < m_ReadersDone || completedReaders >= frame)
        return false;

    m_Frame = frame;
    m_ReadersDone = completedReaders;
    GatherDemand(view);
    GatherShadowDemand(shadowPages);
    BuildReadOrder();
    out.WantsDetails = !m_Priority.empty();
    m_Residency.BeginFrame(frame);
    for (uint32 slotIndex = 0; slotIndex < m_Slots.size(); ++slotIndex)
    {
        Slot& slot = m_Slots[slotIndex];
        if (slot.Current == Stage::Empty)
            continue;
        const uint32 page = slot.Ticket.Page;
        const bool wanted = m_Wanted[page] != 0 || m_ShadowMasks[page] != 0;
        if (wanted)
            slot.ShadowOnly = IsShadowOnly(page);
        if (wanted && ((slot.Current == Stage::Ready && slot.LastReader != 0) || slot.Current == Stage::Uploading))
        {
            // A verified page is still ours. A returning view draws it again, and EmitDraws
            // extends its fence before the slot can be reused.
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
            ReleaseIfRetired(slotIndex);
    }
    AppendReads(out.Reads);
    out.DetailBudget = m_DetailBudget;
    EmitDraws(frame, out);
    return true;
}

void ForestPlacementRuntime::AppendReads(std::vector<ForestPageTicket>& reads)
{
    const uint32 physicalShadowSlots = static_cast<uint32>(m_Slots.size()) - m_DetailCapacity - m_TransitionCapacity;
    uint32 shadowSlots = CountShadowSlots();
    for (uint32 page : m_ReadOrder)
    {
        if (reads.size() >= m_ReadsPerFrame)
            break;
        // A page whose read failed waits for its retry frame.
        if (m_Frame < m_RetryAt[page] || m_Residency.Find(page).IsResident())
            continue;
        // Shadow pages hold at most ShadowSlots slots, retiring ones included, so the view always
        // finds its detail and transition slots. A full shadow set reuses its own retired slots.
        const bool shadowOnly = IsShadowOnly(page);
        if (shadowOnly && shadowSlots >= m_ShadowCapacity)
        {
            const uint32 victim = FindReusableShadowSlot();
            if (victim == m_Slots.size())
                continue;
            ReleaseIfRetired(victim);
            --shadowSlots;
        }
        // An unwanted page stays warm in its slot until a read needs the slot. A prefetch read keeps
        // the transition slots and the unused shadow slots; free slots and reusable slots count alike.
        const uint32 freeSlots = m_Residency.FreeCount();
        if (m_Wanted[page] == kPrefetchDetail && !shadowOnly &&
            freeSlots + CountReusableSlots() <=
                m_Slots.size() - m_DetailCapacity - std::min(shadowSlots, physicalShadowSlots))
            break;
        if (freeSlots == 0)
        {
            const uint32 victim = FindReusableSlot();
            if (victim == m_Slots.size())
                break;
            if (m_Slots[victim].ShadowOnly)
                --shadowSlots;
            ReleaseIfRetired(victim);
        }
        const auto ref = m_Residency.Acquire(page, 0.0f);
        assert(ref.IsResident() && m_Slots[ref.Slot].Current == Stage::Empty);
        const ForestPageTicket ticket{ref.Slot, page, ref.Generation};
        m_Slots[ref.Slot] = Slot{ticket, Stage::Reading};
        m_Slots[ref.Slot].ShadowOnly = shadowOnly;
        if (shadowOnly)
            ++shadowSlots;
        reads.push_back(ticket);
    }
}

uint32 ForestPlacementRuntime::FindReusableSlot() const
{
    uint32 victim = static_cast<uint32>(m_Slots.size());
    uint64 oldest = std::numeric_limits<uint64>::max();
    for (uint32 slotIndex = 0; slotIndex < m_Slots.size(); ++slotIndex)
    {
        const Slot& slot = m_Slots[slotIndex];
        if (slot.Current == Stage::Ready && slot.Retiring && slot.LastReader <= m_ReadersDone &&
            slot.LastReader < oldest)
        {
            victim = slotIndex;
            oldest = slot.LastReader;
        }
    }
    return victim;
}

uint32 ForestPlacementRuntime::FindReusableShadowSlot() const
{
    uint32 victim = static_cast<uint32>(m_Slots.size());
    uint64 oldest = std::numeric_limits<uint64>::max();
    for (uint32 slotIndex = 0; slotIndex < m_Slots.size(); ++slotIndex)
    {
        const Slot& slot = m_Slots[slotIndex];
        if (slot.ShadowOnly && slot.Current == Stage::Ready && slot.Retiring && slot.LastReader <= m_ReadersDone &&
            slot.LastReader < oldest)
        {
            victim = slotIndex;
            oldest = slot.LastReader;
        }
    }
    return victim;
}

uint32 ForestPlacementRuntime::CountShadowSlots() const
{
    return static_cast<uint32>(std::ranges::count_if(m_Slots, [](const Slot& slot)
        { return slot.ShadowOnly && slot.Current != Stage::Empty; }));
}

uint32 ForestPlacementRuntime::CountReusableSlots() const
{
    return static_cast<uint32>(std::ranges::count_if(m_Slots, [this](const Slot& slot)
        { return slot.Current == Stage::Ready && slot.Retiring && slot.LastReader <= m_ReadersDone; }));
}

void ForestPlacementRuntime::DelayRetry(uint32 page)
{
    m_RetryAt[page] = PageStreaming::NextPageFetchRetryFrame(m_Frame, m_FailedReads[page]++);
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
    constexpr uint16 kLargestOffset = std::numeric_limits<uint16>::max();
    ForestPlacementRootBounds roots{kLargestOffset, kLargestOffset, kLargestOffset, 0, 0, 0};
    for (size_t offset = 0; offset < bytes.size(); offset += sizeof(PageStreaming::PackedPlacement))
    {
        PageStreaming::PackedPlacement packed;
        std::memcpy(&packed, bytes.data() + offset, sizeof(packed));
        roots.MinX = std::min(roots.MinX, packed.X);
        roots.MinY = std::min(roots.MinY, packed.Height);
        roots.MinZ = std::min(roots.MinZ, packed.Z);
        roots.MaxX = std::max(roots.MaxX, packed.X);
        roots.MaxY = std::max(roots.MaxY, packed.Height);
        roots.MaxZ = std::max(roots.MaxZ, packed.Z);
    }
    slot.RootBounds = roots;
    if (m_KeepCpuCopy)
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

bool ForestPlacementRuntime::CompleteUpload(ForestPageTicket ticket, ForestUploadResult result)
{
    if (ticket.Slot >= m_Slots.size())
        return false;
    Slot& slot = m_Slots[ticket.Slot];
    if (slot.Ticket != ticket || slot.Current != Stage::Uploading)
        return false;
    slot.Current = Stage::Ready;
    if (!slot.Retiring && result == ForestUploadResult::Published)
    {
        m_FailedReads[ticket.Page] = 0;
        return true;
    }
    if (!slot.Retiring && result == ForestUploadResult::Failed)
        DelayRetry(ticket.Page);
    slot.Retiring = true;
    ReleaseIfRetired(ticket.Slot);
    return false;
}

void ForestPlacementRuntime::EmitStand(uint32 first, uint32 end, std::vector<DrawSubCommand>& draws) const
{
    if (first == end)
        return;
    draws.push_back({.IndexCount = (end - first) * kIndicesPerPage, .FirstIndex = first * kIndicesPerPage});
}

float ForestPlacementRuntime::HandoffWeight(uint32 page) const
{
    const ForestPlacementView& view = *m_DemandView;
    if (!(view.HandoffWidth > 0.0f))
        return 1.0f;
    const Bounds& box = m_Leaves[page];
    const double distance = std::sqrt(DistanceSquared(box.Min, box.Max, view.Eye));
    // A drawn page is nearer than the reach, so its weight is above 0.
    const double weight = (DetailReach(view) - distance) / view.HandoffWidth;
    return std::clamp(static_cast<float>(weight), std::numeric_limits<float>::min(), 1.0f);
}

void ForestPlacementRuntime::EmitDraws(uint64 frame, ForestPlacementFrame& out)
{
    m_Ready.clear();
    for (uint32 slotIndex = 0; slotIndex < m_Slots.size(); ++slotIndex)
    {
        Slot& slot = m_Slots[slotIndex];
        if (slot.Current != Stage::Ready || slot.Retiring)
            continue;
        const uint32 page = slot.Ticket.Page;
        if (m_Wanted[page] >= kDrawHandoff)
        {
            slot.LastReader = frame;
            m_Ready.emplace_back(page, slotIndex);
        }
        // A cascade pass reads the slot in this frame too, so the same fence guards its reuse.
        if (m_ShadowMasks[page] != 0)
        {
            slot.LastReader = frame;
            out.ShadowDraws.push_back(
                {{slot.Ticket, slotIndex * kPlacementsPerSlot, m_Store.Pages()[page].Count}, m_ShadowMasks[page]});
        }
    }
    std::ranges::sort(m_Ready);
    auto ready = m_Ready.begin();
    for (const auto& run : m_Visible)
    {
        const uint32 end = run.First + run.Count;
        uint32 cursor = run.First;
        out.VisiblePages += run.Count;
        while (ready != m_Ready.end() && ready->first < end)
        {
            const auto [page, slot] = *ready++;
            assert(cursor <= page);
            EmitStand(cursor, page, out.StandDraws);
            const float weight = HandoffWeight(page);
            if (m_Wanted[page] == kDrawDetail)
                ++out.DetailBudget.Resident;
            out.DetailedDraws.push_back({m_Slots[slot].Ticket, slot * kPlacementsPerSlot, m_Store.Pages()[page].Count,
                                         weight});
            if (weight < 1.0f)
                out.StandFades.push_back({{.IndexCount = kIndicesPerPage, .FirstIndex = page * kIndicesPerPage}, weight});
            cursor = page + 1;
        }
        EmitStand(cursor, end, out.StandDraws);
    }
    assert(ready == m_Ready.end());
}

} // namespace GameEngine::Engine::Renderer
