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

bool BuildForestPlacementGpuInputs(const ForestPlacementRuntime& runtime, const ForestPlacementFrame& plan,
    const Rendering::CameraData& camera, uint32 slotCount, float rootRadius,
    const Mathematics::Vector4& atlasBounds, ForestPlacementGpuView& view,
    std::vector<ForestPlacementGpuPage>& pages, std::span<const ForestMeshOwnedPlacement> meshOwned)
{
    constexpr uint32 kSlotSize = PageStreaming::PackedPlacementStore::kPlacementsPerPage;
    pages.clear();
    if (camera.renderOriginSector[3] == 0 || plan.DetailedDraws.size() > slotCount ||
        !std::isfinite(rootRadius) || rootRadius <= 0)
        return false;

    const auto archive = runtime.Store().Pages();
    const auto origin = Mathematics::WorldPosition::FromSector(CameraRenderOriginSector(camera), {});
    Rendering::ExtractFrustumPlanes(camera.viewProjRel, view.Planes.data());
    pages.reserve(plan.DetailedDraws.size());
    for (const auto& detail : plan.DetailedDraws)
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
                          ForestHandoffFadeCode(detail.DetailWeight, false)}, {local, 0.0f}, {}});
    }
    // EmitDraws keeps detail pages in page order. Resolve a mesh claim against
    // its complete ticket so an old mesh cannot suppress a replacement slot.
    for (const auto& owner : meshOwned)
    {
        const auto found = std::ranges::lower_bound(plan.DetailedDraws, owner.Ticket.Page, {},
            [](const ForestDetailDraw& draw) { return draw.Ticket.Page; });
        if (found == plan.DetailedDraws.end() || found->Ticket != owner.Ticket ||
            owner.LocalPlacement >= found->PlacementCount)
            continue;
        auto& page = pages[static_cast<size_t>(found - plan.DetailedDraws.begin())];
        page.MeshOwned[owner.LocalPlacement / 32] |= 1u << (owner.LocalPlacement % 32);
    }
    const Mathematics::Vector3 eye = CameraEyeRelativeToOrigin(camera);
    view.ViewProjectionRelative = camera.viewProjRel;
    view.EyeRelativeAndRadius = {eye, rootRadius};
    view.AtlasBounds = atlasBounds;
    return true;
}

} // namespace GameEngine::Engine::Renderer
