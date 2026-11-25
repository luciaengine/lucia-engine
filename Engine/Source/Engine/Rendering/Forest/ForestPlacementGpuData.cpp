#include "ForestPlacementGpuData.h"

#include "Engine/Rendering/RenderOrigin.h"
#include "Rendering/Common/Frustum.h"
#include "Rendering/Core/GPUDrawStreamBuilder.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace GameEngine::Engine::Renderer
{

namespace
{

bool RootsInsideFrustum(const ForestPlacementRootBounds& roots, const Mathematics::Vector3& origin,
                        const std::array<Mathematics::Vector4, 6>& planes)
{
    constexpr double centimeters = 0.01;
    constexpr double marginFactor = 8 * std::numeric_limits<float>::epsilon();
    for (const Mathematics::Vector4& plane : planes)
    {
        const double xOffset = (plane.x >= 0 ? roots.MinX : roots.MaxX) * centimeters;
        const double yOffset = (plane.y >= 0 ? roots.MinY : roots.MaxY) * centimeters;
        const double zOffset = (plane.z >= 0 ? roots.MinZ : roots.MaxZ) * centimeters;
        const double x = (static_cast<double>(origin.x) + xOffset) * plane.x;
        const double y = (static_cast<double>(origin.y) + yOffset) * plane.y;
        const double z = (static_cast<double>(origin.z) + zOffset) * plane.z;
        // Include root reconstruction error even where origin and local offset cancel.
        const double margin = marginFactor *
            ((std::abs(origin.x) + xOffset) * std::abs(plane.x) + (std::abs(origin.y) + yOffset) * std::abs(plane.y) +
             (std::abs(origin.z) + zOffset) * std::abs(plane.z) + std::abs(plane.w));
        if (!(x + y + z + plane.w > margin))
            return false;
    }
    return true;
}

float PageBoundAxis(double originOffset, float localBound, float direction)
{
    // The vertex shader rounds the page origin and local position separately.
    constexpr double kRoundingFactor = 8 * std::numeric_limits<float>::epsilon();
    const double margin = kRoundingFactor * (std::abs(originOffset) + std::abs(localBound));
    return std::nextafter(static_cast<float>(originOffset + localBound + margin * direction),
                          direction * std::numeric_limits<float>::infinity());
}

Mathematics::Vector4 PageBound(const Mathematics::WorldPosition& pageOrigin,
                              const Mathematics::WorldPosition& viewOrigin,
                              const Mathematics::Vector3& offset, float direction)
{
    return {PageBoundAxis(pageOrigin.X - viewOrigin.X, offset.x, direction),
            PageBoundAxis(pageOrigin.Y - viewOrigin.Y, offset.y, direction),
            PageBoundAxis(pageOrigin.Z - viewOrigin.Z, offset.z, direction), 0.0f};
}

} // namespace

uint32 ForestHandoffFadeCode(float detailWeight, bool overview)
{
    using Fade = Rendering::GPUDrawStreamBuilder;
    if (!(detailWeight < 1.0f))
        return 0;
    // Weight 0 means not fading, so the faintest band keeps one level.
    const auto level = static_cast<uint32>(std::clamp(std::lround(detailWeight * Fade::kLodFadeMaxWeight), 1l,
                                                      static_cast<long>(Fade::kLodFadeMaxWeight)));
    return (level << Fade::kLodFadeWeightShift) | Fade::kLodFadeGradientBit | (overview ? Fade::kLodFadePhaseBit : 0u);
}

std::optional<ForestPlacementGpuViewSource> MakeForestPlacementGpuViewSource(const Rendering::CameraData& camera)
{
    if (camera.renderOriginSector[3] == 0)
        return std::nullopt;
    ForestPlacementGpuViewSource source{camera.viewProjRel, {}, CameraEyeRelativeToOrigin(camera),
                                        CameraRenderOriginSector(camera)};
    Rendering::ExtractFrustumPlanes(camera.viewProjRel, source.Planes.data());
    return source;
}

ForestPlacementGpuViewSource MakeForestShadowGpuViewSource(const DepthEmitCascades& cascades, uint32 cascade)
{
    const DepthEmitCascade& fit = cascades.Cascades[cascade];
    return {fit.LightViewProjectionRel, fit.CasterPlanes, {}, cascades.RenderOriginSector};
}

bool BuildForestPlacementGpuInputs(const ForestPlacementRuntime& runtime, std::span<const ForestDetailDraw> draws,
    const ForestPlacementGpuViewSource& source, uint32 slotCount, float rootRadius,
    const Mathematics::Vector4& atlasBounds, ForestPlacementGpuView& view,
    std::vector<ForestPlacementGpuPage>& pages, std::span<const ForestMeshOwnedPlacement> meshOwned)
{
    constexpr uint32 kSlotSize = PageStreaming::PackedPlacementStore::kPlacementsPerPage;
    pages.clear();
    if (draws.size() > slotCount || !std::isfinite(rootRadius) || rootRadius <= 0)
        return false;

    const auto archive = runtime.Store().Pages();
    const auto origin = Mathematics::WorldPosition::FromSector(source.Sector, {});
    view.Planes = source.Planes;
    pages.reserve(draws.size());
    for (const auto& detail : draws)
    {
        const uint32 page = detail.Ticket.Page;
        if (page >= archive.size() || detail.Ticket.Slot >= slotCount ||
            detail.FirstPlacement != uint64{detail.Ticket.Slot} * kSlotSize ||
            detail.PlacementCount != archive[page].Count)
        {
            pages.clear();
            return false;
        }

        const Mathematics::Vector3 local = archive[page].Origin - origin;
        if (!std::isfinite(local.x) || !std::isfinite(local.y) || !std::isfinite(local.z))
        {
            pages.clear();
            return false;
        }
        const auto roots = runtime.ReadyRootBounds(detail.Ticket);
        const uint32 inside = roots && RootsInsideFrustum(*roots, local, view.Planes) ? 1u : 0u;
        pages.push_back({{detail.FirstPlacement, detail.PlacementCount, inside,
                          ForestHandoffFadeCode(detail.DetailWeight, false)}, {local, 0.0f}, {},
                         PageBound(archive[page].Origin, origin, archive[page].LocalMin, -1.0f),
                         PageBound(archive[page].Origin, origin, archive[page].LocalMax, 1.0f)});
    }
    // Draws with mesh claims are in page order. Resolve a mesh claim against
    // its complete ticket so an old mesh cannot suppress a replacement slot.
    for (const auto& owner : meshOwned)
    {
        const auto found = std::ranges::lower_bound(draws, owner.Ticket.Page, {},
            [](const ForestDetailDraw& draw) { return draw.Ticket.Page; });
        if (found == draws.end() || found->Ticket != owner.Ticket ||
            owner.LocalPlacement >= found->PlacementCount)
            continue;
        auto& page = pages[static_cast<size_t>(found - draws.begin())];
        page.MeshOwned[owner.LocalPlacement / 32] |= 1u << (owner.LocalPlacement % 32);
    }
    view.ViewProjectionRelative = source.ViewProjectionRelative;
    view.EyeRelativeAndRadius = {source.EyeRelative, rootRadius};
    view.AtlasBounds = atlasBounds;
    return true;
}

} // namespace GameEngine::Engine::Renderer
