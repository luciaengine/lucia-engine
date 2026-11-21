#include "Engine/Rendering/VirtualShadows/VirtualShadowPageCache.h"

#include <algorithm>
#include <bit>
#include <tuple>

namespace GameEngine::Engine::Renderer
{

namespace
{

// Marks a row that repeats an earlier request. Plan removes it before it returns.
constexpr uint32_t kRepeatedRow = kUnmappedShadowPage - 1;

bool IsValidPage(const VirtualShadowPage& page)
{
    switch (page.Family)
    {
    case VirtualShadowLight::Directional:
        return page.Projection < kVirtualShadowClipLevels && page.Mip == 0;
    case VirtualShadowLight::Spot:
        if (page.Projection != 0) return false;
        break;
    case VirtualShadowLight::Point:
        if (page.Projection >= 6) return false;
        break;
    default:
        return false;
    }

    if (page.Mip > std::countr_zero(kVirtualLocalShadowPagesPerAxis) || page.X < 0 || page.Y < 0)
        return false;
    const int64_t side = kVirtualLocalShadowPagesPerAxis >> page.Mip;
    return page.X < side && page.Y < side;
}

} // namespace

std::optional<Mathematics::Matrix4x4> CropVirtualShadowProjection(
    const Mathematics::Matrix4x4& lightProjection, uint32_t pagesPerAxis, uint32_t x, uint32_t y)
{
    if (!std::has_single_bit(pagesPerAxis) || x >= pagesPerAxis || y >= pagesPerAxis)
        return std::nullopt;
    const auto scale = static_cast<float>(pagesPerAxis);
    auto crop = Mathematics::Matrix4x4::Identity();
    crop[0][0] = scale;
    crop[1][1] = scale;
    crop[3][0] = scale - 1.0f - 2.0f * static_cast<float>(x);
    crop[3][1] = 1.0f - scale + 2.0f * static_cast<float>(y);
    return crop * lightProjection;
}

VirtualShadowPageCache::VirtualShadowPageCache(uint32_t tileCount)
    : m_Tiles(tileCount)
{
    m_Mappings.Reserve(tileCount);
}

std::vector<VirtualShadowPageWork> VirtualShadowPageCache::Plan(
    std::span<const VirtualShadowPage> requests, uint64_t frame, uint64_t completedFrame)
{
    const uint64_t stamp = ++m_PlanStamp;
    std::vector<VirtualShadowPageWork> work;
    work.reserve(requests.size());
    m_NewRows.clear();
    for (const auto& page : requests)
    {
        if (!IsValidPage(page))
            continue;
        if (const auto* found = m_Mappings.Find(page))
        {
            auto& tile = m_Tiles[*found];
            if (tile.PlanStamp == stamp)
                continue;
            tile.PlanStamp = stamp;
            work.push_back({.Page = page, .Tile = *found});
            continue;
        }
        m_NewRows.push_back(static_cast<uint32_t>(work.size()));
        work.push_back({.Page = page});
    }
    RemoveRepeatedNewPages(work);

    // A tile keeps the stamp only while a row in the resident part names its page.
    const auto residentCount = std::min(work.size(), m_Tiles.size());
    for (auto& row : std::span(work).subspan(residentCount))
    {
        if (row.Tile == kUnmappedShadowPage)
            continue;
        m_Tiles[row.Tile].PlanStamp = 0;
        row.Tile = kUnmappedShadowPage;
    }
    const auto resident = std::span(work).first(residentCount);
    MapNewPages(resident, stamp, completedFrame);

    for (auto& row : resident)
    {
        if (row.Tile == kUnmappedShadowPage)
            continue;
        auto& tile = m_Tiles[row.Tile];
        tile.LastUse = frame;
        row.Revision = tile.Revision;
        row.NeedsRender = !tile.Rendered;
        if (row.NeedsRender)
            tile.RenderFrame = frame;
    }
    RefreshDeformed(resident, frame);
    return work;
}

void VirtualShadowPageCache::RemoveRepeatedNewPages(std::vector<VirtualShadowPageWork>& work)
{
    if (m_NewRows.size() < 2)
        return;
    std::ranges::sort(m_NewRows, [&work](uint32_t a, uint32_t b) {
        return std::tie(work[a].Page, a) < std::tie(work[b].Page, b);
    });
    bool repeated = false;
    for (std::size_t index = 1; index < m_NewRows.size(); ++index)
    {
        auto& row = work[m_NewRows[index]];
        if (row.Page != work[m_NewRows[index - 1]].Page)
            continue;
        row.Tile = kRepeatedRow;
        repeated = true;
    }
    if (repeated)
        std::erase_if(work, [](const VirtualShadowPageWork& row) { return row.Tile == kRepeatedRow; });
}

void VirtualShadowPageCache::MapNewPages(std::span<VirtualShadowPageWork> rows, uint64_t stamp, uint64_t completedFrame)
{
    if (std::ranges::none_of(rows, [](const auto& row) { return row.Tile == kUnmappedShadowPage; }))
        return;
    std::vector<uint32_t> available;
    available.reserve(m_Tiles.size());
    for (uint32_t index = 0; index < m_Tiles.size(); ++index)
    {
        const auto& tile = m_Tiles[index];
        if (!tile.Page || (tile.LastUse <= completedFrame && tile.PlanStamp != stamp))
            available.push_back(index);
    }
    std::ranges::sort(available, {}, [this](uint32_t index) {
        const auto& tile = m_Tiles[index];
        return std::tuple{tile.Page.has_value(), tile.LastUse, index};
    });

    std::size_t next = 0;
    for (auto& row : rows)
    {
        if (row.Tile != kUnmappedShadowPage)
            continue;
        if (next == available.size())
            return;
        row.Tile = available[next++];
        auto& tile = m_Tiles[row.Tile];
        if (tile.Page)
            m_Mappings.Erase(*tile.Page);
        tile.Page = row.Page;
        tile.PlanStamp = stamp;
        Invalidate(tile);
        m_Mappings.InsertOrAssign(row.Page, row.Tile);
    }
}

void VirtualShadowPageCache::RefreshDeformed(std::span<VirtualShadowPageWork> rows, uint64_t frame)
{
    const auto missing = static_cast<uint32_t>(std::ranges::count_if(rows, &VirtualShadowPageWork::NeedsRender));
    if (missing >= kVirtualShadowPageRenderBudget)
        return;
    struct Candidate
    {
        VirtualShadowPageWork* Row;
        double Staleness;
    };
    std::vector<Candidate> deformed;
    for (std::size_t index = 0; index < rows.size(); ++index)
    {
        auto& row = rows[index];
        if (row.Tile == kUnmappedShadowPage || row.NeedsRender || !m_Tiles[row.Tile].Deformed)
            continue;
        // The clip level of a sun page and the mip of a local page both grow with the texel size.
        const uint32_t level = row.Page.Family == VirtualShadowLight::Directional ? row.Page.Projection : row.Page.Mip;
        const uint64_t age = frame - std::min(frame, m_Tiles[row.Tile].RenderFrame);
        // Rows come nearest first, so a page later in the order may wait longer.
        const double limit = static_cast<double>(kVirtualShadowDeformedMaxAgeFrames << std::min(level, 16u)) *
                             (1.0 + static_cast<double>(index) / kVirtualShadowDeformedRefreshRowScale);
        deformed.push_back({&row, static_cast<double>(age) / limit});
    }
    const auto count = std::min<std::size_t>(
        deformed.size(), std::min(kVirtualShadowDeformedRefreshBudget, kVirtualShadowPageRenderBudget - missing));
    std::ranges::partial_sort(deformed, deformed.begin() + static_cast<std::ptrdiff_t>(count), {},
                              [](const Candidate& candidate) { return -candidate.Staleness; });
    for (const auto& candidate : std::span(deformed).first(count))
    {
        auto& tile = m_Tiles[candidate.Row->Tile];
        Invalidate(tile);
        tile.RenderFrame = frame;
        candidate.Row->Revision = tile.Revision;
        candidate.Row->NeedsRender = true;
        candidate.Row->Refresh = true;
    }
}

bool VirtualShadowPageCache::MarkRendered(const VirtualShadowPageWork& work)
{
    if (!work.NeedsRender || !Contains(work))
        return false;
    auto& tile = m_Tiles[work.Tile];
    tile.Rendered = true;
    return true;
}

bool VirtualShadowPageCache::Contains(const VirtualShadowPageWork& work) const
{
    return work.Tile < m_Tiles.size() && m_Tiles[work.Tile].Page == work.Page &&
           m_Tiles[work.Tile].Revision == work.Revision;
}

void VirtualShadowPageCache::Invalidate(Tile& tile)
{
    tile.Revision = m_NextRevision++;
    tile.Rendered = false;
    tile.Deformed = false;
}

void VirtualShadowPageCache::MarkDirty(const VirtualShadowPage& page)
{
    if (const auto* found = m_Mappings.Find(page))
        Invalidate(m_Tiles[*found]);
}

void VirtualShadowPageCache::InvalidateLight(VirtualShadowLight family, uint64_t lightId)
{
    InvalidatePages([=](const auto& page) { return page.Family == family && page.LightId == lightId; });
}

void VirtualShadowPageCache::InvalidateProjection(const VirtualShadowPage& origin)
{
    InvalidatePages([&origin](const auto& page) {
        return page.Family == origin.Family && page.LightId == origin.LightId &&
               page.Projection == origin.Projection;
    });
}

void VirtualShadowPageCache::InvalidateAll()
{
    InvalidatePages([](const auto&) { return true; });
}

void VirtualShadowPageCache::MarkDeformed()
{
    for (auto& tile : m_Tiles)
        tile.Deformed = tile.Rendered;
}

} // namespace GameEngine::Engine::Renderer
