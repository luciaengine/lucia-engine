#include "Engine/Rendering/Forest/ForestPlacementGpuData.h"

#include "Engine/Rendering/RenderOrigin.h"
#include "Rendering/Common/Frustum.h"

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
    for (const auto& p : planes)
    {
        const double xOffset = (p.x >= 0 ? roots.MinX : roots.MaxX) * centimeters;
        const double zOffset = (p.z >= 0 ? roots.MinZ : roots.MaxZ) * centimeters;
        const double x = (static_cast<double>(origin.x) + xOffset) * p.x;
        const double y = static_cast<double>(origin.y) * p.y;
        const double z = (static_cast<double>(origin.z) + zOffset) * p.z;
        // Include root reconstruction error even where origin and local offset cancel.
        const double margin = marginFactor *
            ((std::abs(origin.x) + xOffset) * std::abs(p.x) + std::abs(y) +
             (std::abs(origin.z) + zOffset) * std::abs(p.z) + std::abs(p.w));
        if (!(x + y + z + p.w > margin))
            return false;
    }
    return true;
}

} // namespace

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

        const auto& coordinates = archive[page].Origin;
        const Mathematics::Vector3 local =
            Mathematics::WorldPosition{coordinates[0], coordinates[1], coordinates[2]} - origin;
        if (!std::isfinite(local.x) || !std::isfinite(local.y) || !std::isfinite(local.z))
        {
            pages.clear();
            return false;
        }
        const auto roots = runtime.ReadyRootBounds(detail.Ticket);
        const uint32 inside = roots && RootsInsideFrustum(*roots, local, view.Planes) ? 1u : 0u;
        pages.push_back({{detail.FirstPlacement, detail.PlacementCount, inside, 0}, {local.x, local.y, local.z, 0}, {}});
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
    std::copy_n(camera.viewProjRel.Data(), view.ViewProjectionRelative.size(), view.ViewProjectionRelative.begin());
    view.EyeRelativeAndRadius = {eye.x, eye.y, eye.z, rootRadius};
    view.AtlasBounds = {atlasBounds.x, atlasBounds.y, atlasBounds.z, atlasBounds.w};
    return true;
}

} // namespace GameEngine::Engine::Renderer
