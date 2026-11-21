#pragma once

#include "Engine/Rendering/Forest/ForestPlacementRuntime.h"
#include "Rendering/Camera/CameraTypes.h"

namespace GameEngine::Engine::Renderer
{

struct alignas(16) ForestPlacementGpuPage
{
    std::array<uint32, 4> Range; // First slot, count, roots fully inside the frustum, padding.
    std::array<float, 4> OriginRelative;
    // One bit per page-local placement. Only a mesh already submitted for this
    // view can claim a bit. The shader reads five of these eight words.
    std::array<uint32, 8> MeshOwned;
};
static_assert(offsetof(ForestPlacementGpuPage, MeshOwned) == 32);
static_assert(sizeof(ForestPlacementGpuPage) == 64);

struct ForestMeshOwnedPlacement
{
    ForestPageTicket Ticket;
    uint32 LocalPlacement;
    auto operator<=>(const ForestMeshOwnedPlacement&) const = default;
};

struct alignas(16) ForestPlacementGpuView
{
    std::array<float, 16> ViewProjectionRelative;
    std::array<Mathematics::Vector4, 6> Planes;
    std::array<float, 4> EyeRelativeAndRadius;
    std::array<float, 4> AtlasBounds;
};
static_assert(sizeof(ForestPlacementGpuView) == 192);

struct ForestPlacementGpuVisible
{
    uint32 Placement;
    uint32 Page;
};
static_assert(sizeof(ForestPlacementGpuVisible) == 8);

struct ForestPlacementIndirectDraw
{
    uint32 IndexCount = 6;
    uint32 InstanceCount = 0;
    uint32 FirstIndex = 0;
    int32 VertexOffset = 0;
    uint32 FirstInstance = 0;
};
static_assert(sizeof(ForestPlacementIndirectDraw) == 20);

/// GPU inputs for one primary view's admitted detail pages. Page positions
/// are rebased in double before converting to floats. rootRadius includes
/// the maximum wind envelope and draw culling margin at unit scale.
/// meshOwned names placements with a complete mesh submission for this view.
/// Only identities matching this frame's ready detail pages can suppress cards.
bool BuildForestPlacementGpuInputs(const ForestPlacementRuntime& runtime, const ForestPlacementFrame& plan,
    const Rendering::CameraData& camera, uint32 slotCount, float rootRadius,
    const Mathematics::Vector4& atlasBounds, ForestPlacementGpuView& view,
    std::vector<ForestPlacementGpuPage>& pages, std::span<const ForestMeshOwnedPlacement> meshOwned = {});

} // namespace GameEngine::Engine::Renderer
