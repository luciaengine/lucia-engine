#pragma once

#include "ForestPlacementRuntime.h"
#include "Engine/Rendering/WindVolumeResolver.h"
#include "Mathematics/Matrix4x4.h"
#include "Mathematics/Vector3Int.h"
#include "Mathematics/Vector4.h"
#include "Rendering/Camera/CameraTypes.h"

#include <optional>

namespace GameEngine::Engine::Renderer
{

struct alignas(16) ForestPlacementGpuPage
{
    std::array<uint32, 4> Range; // First slot, count, roots fully inside the frustum, handoff fade code.
    Mathematics::Vector4 OriginRelative;
    // One bit per page-local placement. Only a mesh already submitted for this
    // view can claim a bit. The shader reads five of these eight words.
    std::array<uint32, 8> MeshOwned;
    Mathematics::Vector4 BoundsMinRelative;
    Mathematics::Vector4 BoundsMaxRelative;
};
static_assert(offsetof(ForestPlacementGpuPage, OriginRelative) == 16);
static_assert(offsetof(ForestPlacementGpuPage, MeshOwned) == 32);
static_assert(offsetof(ForestPlacementGpuPage, BoundsMinRelative) == 64);
static_assert(offsetof(ForestPlacementGpuPage, BoundsMaxRelative) == 80);
static_assert(sizeof(ForestPlacementGpuPage) == 96);
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

/// One visible tree of a splat draw: its placement and its wind sway in meters at full bend.
struct ForestPlacementGpuVisibleSplat
{
    ForestPlacementGpuVisible Placement;
    Mathematics::Vector2 Sway;
};
static_assert(sizeof(ForestPlacementGpuVisibleSplat) == 16);

/// The wind that the splat cull resolves once per visible tree (forest_placement_cull.glsl).
struct alignas(16) ForestSplatWindGpu
{
    std::array<int32, 4> Sector;
    float TimeSeconds = 0.0f;
    float MaxDisplacement = 0.0f;
    Mathematics::Vector2 Padding{};
    WeatherWindGPU Weather;
};
static_assert(offsetof(ForestSplatWindGpu, Weather) == 32);

struct ForestPlacementIndirectDraw
{
    uint32 IndexCount = 6;
    uint32 InstanceCount = 0;
    uint32 FirstIndex = 0;
    int32 VertexOffset = 0;
    uint32 FirstInstance = 0;
};
static_assert(sizeof(ForestPlacementIndirectDraw) == 20);

/// The LOD crossfade code (lod_crossfade.glsl) of a page in the handoff band: 0 for a weight of 1. The trees
/// keep the weight's share of the pixels; the overview quad (overview true) keeps the other pixels.
uint32 ForestHandoffFadeCode(float detailWeight, bool overview);

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

/// GPU inputs for the resident pages of `draws`, rebased in double. rootRadius includes the wind envelope
/// and the cull margin at unit scale. meshOwned names placements whose mesh this view drew; a claim
/// suppresses a card only on a draw with its ticket, and draws are then in page order.
bool BuildForestPlacementGpuInputs(const ForestPlacementRuntime& runtime, std::span<const ForestDetailDraw> draws,
    const ForestPlacementGpuViewSource& source, uint32 slotCount, float rootRadius,
    const Mathematics::Vector4& atlasBounds, ForestPlacementGpuView& view,
    std::vector<ForestPlacementGpuPage>& pages, std::span<const ForestMeshOwnedPlacement> meshOwned = {});

} // namespace GameEngine::Engine::Renderer
