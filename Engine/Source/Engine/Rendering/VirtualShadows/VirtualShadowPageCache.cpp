#include "Engine/Rendering/VirtualShadows/VirtualShadowPageCache.h"

#include "Types/Fnv1a.h"

#include <algorithm>
#include <bit>
#include <tuple>
#include <unordered_set>

namespace GameEngine::Engine::Renderer
{

namespace
{

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

std::size_t VirtualShadowPageCache::PageHash::operator()(const VirtualShadowPage& page) const noexcept
{
    auto hash = Hashing::Fnv1a64Value(Hashing::kFnv1a64OffsetBasis, page.LightId);
    hash = Hashing::Fnv1a64Value(hash, page.Family);
    hash = Hashing::Fnv1a64Value(hash, page.Projection);
    hash = Hashing::Fnv1a64Value(hash, page.Mip);
    hash = Hashing::Fnv1a64Value(hash, page.X);
    return static_cast<std::size_t>(Hashing::Fnv1a64Value(hash, page.Y));
}

VirtualShadowPageCache::VirtualShadowPageCache(uint32_t tileCount)
    : m_Tiles(tileCount)
{
    m_Mappings.Reserve(tileCount);
}

std::vector<VirtualShadowPageWork> VirtualShadowPageCache::Plan(
    std::span<const VirtualShadowPage> requests, uint64_t frame, uint64_t completedFrame)
{
    std::unordered_set<VirtualShadowPage, PageHash> demanded;
    demanded.reserve(requests.size());
    std::vector<VirtualShadowPageWork> work;
    work.reserve(requests.size());
    for (const auto& page : requests)
    {
        if (IsValidPage(page) && demanded.insert(page).second)
            work.push_back({.Page = page});
    }
    const auto residentCount = std::min(work.size(), m_Tiles.size());
    for (std::size_t i = residentCount; i < work.size(); ++i)
        demanded.erase(work[i].Page);

    std::vector<uint32_t> available;
    available.reserve(m_Tiles.size());
    for (uint32_t index = 0; index < m_Tiles.size(); ++index)
    {
        const auto& tile = m_Tiles[index];
        if (!tile.Page || (tile.LastUse <= completedFrame && !demanded.contains(*tile.Page)))
            available.push_back(index);
    }
    std::ranges::sort(available, {}, [this](uint32_t index) {
        const auto& tile = m_Tiles[index];
        return std::tuple{tile.Page.has_value(), tile.LastUse, index};
    });

    std::size_t next = 0;
    for (auto& row : std::span(work).first(residentCount))
    {
        const auto& page = row.Page;
        const auto* found = m_Mappings.Find(page);
        if (found)
        {
            row.Tile = *found;
        }
        else if (next < available.size())
        {
            row.Tile = available[next++];
            auto& tile = m_Tiles[row.Tile];
            if (tile.Page)
                m_Mappings.Erase(*tile.Page);
            tile.Page = page;
            Invalidate(tile);
            m_Mappings.InsertOrAssign(page, row.Tile);
        }
        if (row.Tile == kUnmappedShadowPage)
            continue;

        auto& tile = m_Tiles[row.Tile];
        tile.LastUse = frame;
        row.Revision = tile.Revision;
        row.NeedsRender = !tile.Rendered;
    }
    return work;
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

} // namespace GameEngine::Engine::Renderer
