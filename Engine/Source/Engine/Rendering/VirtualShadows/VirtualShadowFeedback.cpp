#include "Engine/Rendering/VirtualShadows/VirtualShadowFeedback.h"

#include "Engine/Rendering/ShadowCasterChanges.h"
#include "Rendering/Common/Frustum.h"

#include "Platform/Environment.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace GameEngine::Engine::Renderer
{

namespace
{

double ToLightAxis(const Mathematics::Vector3& axis, const Mathematics::WorldPosition& position)
{
    return axis.x * position.X + axis.y * position.Y + axis.z * position.Z;
}

Mathematics::Matrix4x4 MakeDirectionalMatrix(
    const VirtualShadowDirectionalGrid& grid, const VirtualShadowPage& origin, uint32_t side,
    const Mathematics::Vector3Int& renderOriginSector)
{
    const Mathematics::WorldPosition renderOrigin{
        static_cast<double>(renderOriginSector.X) * Mathematics::kWorldSectorSize,
        static_cast<double>(renderOriginSector.Y) * Mathematics::kWorldSectorSize,
        static_cast<double>(renderOriginSector.Z) * Mathematics::kWorldSectorSize,
    };
    const double halfSide = static_cast<double>(side) * 0.5;
    const double centerX = (static_cast<double>(origin.X) + halfSide) * grid.PageWorldSize;
    const double centerY = -(static_cast<double>(origin.Y) + halfSide) * grid.PageWorldSize;
    const double farZ = (static_cast<double>(grid.DepthBand) + kVirtualShadowDirectionalDepthBands * 0.5) *
                        grid.DepthStep;
    const double xyScale = 2.0 / (side * grid.PageWorldSize);
    const double zScale = 1.0 / (grid.DepthStep * kVirtualShadowDirectionalDepthBands);
    const auto& axes = grid.LightAxes;

    auto matrix = Mathematics::Matrix4x4::Identity();
    for (size_t column = 0; column < 3; ++column)
    {
        matrix[column][0] = static_cast<float>(axes[0][column] * xyScale);
        matrix[column][1] = static_cast<float>(axes[1][column] * xyScale);
        matrix[column][2] = static_cast<float>(-axes[2][column] * zScale);
    }
    matrix[3][0] = static_cast<float>((ToLightAxis(axes[0], renderOrigin) - centerX) * xyScale);
    matrix[3][1] = static_cast<float>((ToLightAxis(axes[1], renderOrigin) - centerY) * xyScale);
    matrix[3][2] = static_cast<float>((farZ - ToLightAxis(axes[2], renderOrigin)) * zScale);
    return matrix;
}

bool SameVirtualShadowLightProjection(const VirtualShadowPage& a, const VirtualShadowPage& b)
{
    return a.LightId == b.LightId && a.Family == b.Family && a.Projection == b.Projection;
}

} // namespace

std::optional<VirtualShadowDirectionalWindow> BuildVirtualShadowDirectionalWindow(
    uint64_t lightId, uint32_t level, uint32_t levelCount, uint32_t pagesPerAxis, float shadowDistance,
    const Mathematics::Matrix4x4& lightRotation, const Mathematics::WorldPosition& worldEye,
    const Mathematics::Vector3Int& renderOriginSector)
{
    if (level >= levelCount || levelCount > kVirtualShadowClipLevels ||
        !std::has_single_bit(pagesPerAxis) || pagesPerAxis > kVirtualLocalShadowPagesPerAxis ||
        !std::isfinite(shadowDistance) || shadowDistance <= 0.0f)
        return std::nullopt;

    VirtualShadowDirectionalGrid grid{};
    for (size_t axis = 0; axis < 3; ++axis)
        for (size_t component = 0; component < 3; ++component)
            grid.LightAxes[axis][component] = lightRotation[component][axis];

    grid.DepthStep = std::max(static_cast<double>(shadowDistance), 1.0);
    const int scale = static_cast<int>(level) - static_cast<int>(levelCount) + 2;
    grid.PageWorldSize = std::ldexp(grid.DepthStep, scale) / pagesPerAxis;
    const double x = std::floor(ToLightAxis(grid.LightAxes[0], worldEye) / grid.PageWorldSize);
    const double y = std::floor(-ToLightAxis(grid.LightAxes[1], worldEye) / grid.PageWorldSize);
    const double z = std::floor(ToLightAxis(grid.LightAxes[2], worldEye) / grid.DepthStep);
    // Double can represent adjacent integer page addresses throughout this range.
    constexpr double kAddressLimit = 0x1p52;
    const auto representable = [](double address) { return std::abs(address) < kAddressLimit; };
    if (!representable(x) || !representable(y) || !representable(z))
        return std::nullopt;

    grid.DepthBand = static_cast<int64_t>(z);
    const VirtualShadowPage origin{lightId, VirtualShadowLight::Directional, static_cast<uint8_t>(level), 0,
                                   static_cast<int64_t>(x) - pagesPerAxis / 2,
                                   static_cast<int64_t>(y) - pagesPerAxis / 2};
    return VirtualShadowDirectionalWindow{
        origin, grid, MakeDirectionalMatrix(grid, origin, pagesPerAxis, Mathematics::Vector3Int{}),
        MakeDirectionalMatrix(grid, origin, pagesPerAxis, renderOriginSector),
    };
}

Mathematics::Matrix4x4 VirtualShadowDirectionalPageMatrix(
    const VirtualShadowDirectionalGrid& grid, const VirtualShadowPage& page,
    const Mathematics::Vector3Int& renderOriginSector)
{
    return MakeDirectionalMatrix(grid, page, 1, renderOriginSector);
}

bool VirtualShadowFeedbackEnabled()
{
    static const bool enabled = Platform::EnvironmentSwitchEnabled("GE_VIRTUAL_SHADOWS", false);
    return enabled;
}

bool SameVirtualShadowProjection(const VirtualShadowFeedbackProjection& a,
                                 const VirtualShadowFeedbackProjection& b)
{
    if (!SameVirtualShadowLightProjection(a.BasePage, b.BasePage) || a.PagesPerAxis != b.PagesPerAxis ||
        a.LastMip != b.LastMip || a.DirectionalGrid != b.DirectionalGrid || a.LocalPosition != b.LocalPosition)
        return false;
    if (a.DirectionalGrid)
        return true;
    if (a.BasePage.Family != VirtualShadowLight::Directional && !a.LocalPosition)
        return false;
    return a.BasePage == b.BasePage &&
           std::equal(a.WorldToLight.Data(), a.WorldToLight.Data() + 16, b.WorldToLight.Data());
}

std::optional<uint32_t> FindVirtualShadowFeedbackBit(
    const VirtualShadowFeedbackProjection& projection, const VirtualShadowPage& page)
{
    const auto& base = projection.BasePage;
    const uint32_t baseSide = projection.PagesPerAxis;
    if (!SameVirtualShadowLightProjection(page, base) ||
        page.Mip > projection.LastMip || !std::has_single_bit(baseSide) ||
        baseSide > kVirtualLocalShadowPagesPerAxis || page.Mip >= std::bit_width(baseSide) ||
        page.X < base.X || page.Y < base.Y)
        return std::nullopt;

    const uint64_t x = static_cast<uint64_t>(page.X) - static_cast<uint64_t>(base.X);
    const uint64_t y = static_cast<uint64_t>(page.Y) - static_cast<uint64_t>(base.Y);
    const uint32_t side = baseSide >> page.Mip;
    if (x >= side || y >= side)
        return std::nullopt;
    uint64_t bit = projection.FirstPage + y * side + x;
    for (uint32_t mip = 0; mip < page.Mip; ++mip)
    {
        const uint32_t mipSide = baseSide >> mip;
        bit += mipSide * mipSide;
    }
    if (bit > std::numeric_limits<uint32_t>::max())
        return std::nullopt;
    return static_cast<uint32_t>(bit);
}

bool VirtualShadowPageTouchedByCasters(const VirtualShadowFeedbackProjection& projection,
                                       const VirtualShadowPage& page,
                                       std::span<const ShadowCasterChangeSphere> changes)
{
    if (!SameVirtualShadowLightProjection(projection.BasePage, page))
        return false;

    std::optional<Mathematics::Matrix4x4> cropped;
    if (projection.DirectionalGrid)
    {
        // Cached world pages can outlive the current window.
        cropped = VirtualShadowDirectionalPageMatrix(*projection.DirectionalGrid, page, Mathematics::Vector3Int{});
    }
    else
    {
        if (!FindVirtualShadowFeedbackBit(projection, page))
            return false;
        const uint32_t side = projection.PagesPerAxis >> page.Mip;
        const auto x = static_cast<uint32_t>(page.X - projection.BasePage.X);
        const auto y = static_cast<uint32_t>(page.Y - projection.BasePage.Y);
        cropped = CropVirtualShadowProjection(projection.WorldToLight, side, x, y);
    }
    if (!cropped)
        return true;

    Mathematics::Vector4 planes[6];
    Rendering::ExtractFrustumPlanes(*cropped, planes);
    if (page.Family == VirtualShadowLight::Directional)
        planes[4] = {0.0f, 0.0f, 0.0f, 1.0f}; // The sun raster clamps near depth.
    return std::ranges::any_of(changes, [&planes](const auto& sphere)
    {
        if (!std::isfinite(sphere.X) || !std::isfinite(sphere.Y) || !std::isfinite(sphere.Z) ||
            !std::isfinite(sphere.Radius) || sphere.Radius < 0.0f)
            return true;

        // World-space light matrices have quantized translations. Be
        // conservative when testing against a page at planetary distance.
        constexpr float kPrecisionUlps = 8.0f * std::numeric_limits<float>::epsilon();
        const float scale = std::max({std::abs(sphere.X), std::abs(sphere.Y), std::abs(sphere.Z), 1.0f});
        return Rendering::TestSphereFrustum({sphere.X, sphere.Y, sphere.Z},
                                             sphere.Radius + scale * kPrecisionUlps, planes);
    });
}

std::vector<VirtualShadowPage> DecodeVirtualShadowFeedback(
    std::span<const uint32_t> words, const VirtualShadowFeedbackLayout& dispatched,
    const VirtualShadowFeedbackLayout& current)
{
    std::vector<VirtualShadowPage> result;
    if (dispatched.World != current.World || dispatched.RenderLayerMask != current.RenderLayerMask ||
        words.size() != dispatched.WordCount)
        return result;

    std::vector<std::vector<VirtualShadowPage>> detail(current.Projections.size());
    size_t maxDetailCount = 0;
    for (size_t i = 0; i < current.Projections.size(); ++i)
    {
        const auto& active = current.Projections[i];
        const auto old = std::ranges::find_if(dispatched.Projections, [&](const auto& row) {
            return SameVirtualShadowProjection(row, active);
        });
        if (old == dispatched.Projections.end())
            continue;

        const uint32_t baseSide = old->PagesPerAxis;
        const auto& origin = old->BasePage;
        if (!std::has_single_bit(baseSide) || baseSide > kVirtualLocalShadowPagesPerAxis ||
            old->LastMip >= static_cast<uint32_t>(std::bit_width(baseSide)) || origin.Mip != 0 ||
            origin.X > std::numeric_limits<int64_t>::max() - baseSide ||
            origin.Y > std::numeric_limits<int64_t>::max() - baseSide)
            continue;

        uint64_t end = old->FirstPage;
        for (uint32_t mip = 0; mip <= old->LastMip; ++mip)
        {
            const uint32_t side = baseSide >> mip;
            end += side * side;
        }
        if (end > uint64_t{words.size()} * 32)
            continue;

        // The bits use the shader's fine-to-coarse layout. Walk it backwards
        // for admission so a requested coarser page can cover missing detail.
        uint64_t next = end;
        for (uint32_t mip = old->LastMip + 1; mip-- > 0;)
        {
            const uint32_t side = baseSide >> mip;
            next -= side * side;
            for (uint32_t offset = 0; offset < side * side; ++offset)
            {
                const uint64_t bit = next + offset;
                if ((words[bit / 32] & (uint32_t{1} << (bit % 32))) == 0)
                    continue;
                VirtualShadowPage page = origin;
                page.Mip = static_cast<uint8_t>(mip);
                page.X += offset % side;
                page.Y += offset / side;
                if (active.DirectionalGrid && !FindVirtualShadowFeedbackBit(active, page))
                    continue;
                if (mip == old->LastMip && origin.Family != VirtualShadowLight::Directional)
                    result.push_back(page);
                else
                    detail[i].push_back(page);
            }
        }
        maxDetailCount = std::max(maxDetailCount, detail[i].size());
    }

    for (size_t rank = 0; rank < maxDetailCount; ++rank)
        for (const auto& pages : detail)
            if (rank < pages.size())
                result.push_back(pages[rank]);
    return result;
}

} // namespace GameEngine::Engine::Renderer
