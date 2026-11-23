#pragma once

#include "ForestPlacementRuntime.h"
#include "Mathematics/Matrix4x4.h"
#include "Mathematics/Vector3Int.h"
#include "Mathematics/Vector4.h"
#include "Rendering/Camera/CameraTypes.h"

#include <optional>

namespace GameEngine::Engine::Renderer
{

struct alignas(16) ForestPlacementGpuPage
{
    std::array<uint32, 4> Range; // First slot, count, roots fully inside the frustum, padding.
    Mathematics::Vector4 OriginRelative;
    // One bit per page-local placement. Only a mesh already submitted for this
    // view can claim a bit. The shader reads five of these eight words.
    std::array<uint32, 8> MeshOwned;
};
static_assert(offsetof(ForestPlacementGpuPage, OriginRelative) == 16);
static_assert(offsetof(ForestPlacementGpuPage, MeshOwned) == 32);
static_assert(sizeof(ForestPlacementGpuPage) == 64);
// The shader holds one page in kForestPlacementsPerSlot (forest_placement.glsl) and reads five MeshOwned words.
static_assert(PageStreaming::PackedPlacementStore::kPlacementsPerPage <= 5 * 32);

struct ForestMeshOwnedPlacement
{
    ForestPageTicket Ticket;
    uint32 LocalPlacement;
    auto operator<=>(const ForestMeshOwnedPlacement&) const = default;
};

struct alignas(16) ForestPlacementGpuView
{
    Mathematics::Matrix4x4 ViewProjectionRelative;
    std::array<Mathematics::Vector4, 6> Planes;
    Mathematics::Vector4 EyeRelativeAndRadius;
    Mathematics::Vector4 AtlasBounds;
};
static_assert(offsetof(ForestPlacementGpuView, Planes) == 64);
static_assert(offsetof(ForestPlacementGpuView, EyeRelativeAndRadius) == 160);
static_assert(offsetof(ForestPlacementGpuView, AtlasBounds) == 176);
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

/// The view that a placement cull and its card draw use, relative to the render origin Sector.
struct ForestPlacementGpuViewSource
{
    Mathematics::Matrix4x4 ViewProjectionRelative;
    /// Normals toward the inside (Rendering::ExtractFrustumPlanes).
    std::array<Mathematics::Vector4, 6> Planes;
    /// The eye. Zero for a sun cascade, which has no eye point.
    Mathematics::Vector3 EyeRelative;
    Mathematics::Vector3Int Sector;
};

/// The camera's view, or none when the camera has no eye (SetCameraEye).
std::optional<ForestPlacementGpuViewSource> MakeForestPlacementGpuViewSource(const Rendering::CameraData& camera);
/// The view of sun cascade `cascade`: its light view-projection and its caster planes.
ForestPlacementGpuViewSource MakeForestShadowGpuViewSource(const DepthEmitCascades& cascades, uint32 cascade);

/// GPU inputs for the resident pages of `draws`. Page positions
/// are rebased in double before converting to floats. rootRadius includes
/// the maximum wind envelope and draw culling margin at unit scale.
/// meshOwned names placements with a complete mesh submission for this view.
/// Only identities matching a draw's ticket can suppress cards; with meshOwned, draws are in page order.
bool BuildForestPlacementGpuInputs(const ForestPlacementRuntime& runtime, std::span<const ForestDetailDraw> draws,
    const ForestPlacementGpuViewSource& source, uint32 slotCount, float rootRadius,
    const Mathematics::Vector4& atlasBounds, ForestPlacementGpuView& view,
    std::vector<ForestPlacementGpuPage>& pages, std::span<const ForestMeshOwnedPlacement> meshOwned = {});

} // namespace GameEngine::Engine::Renderer
