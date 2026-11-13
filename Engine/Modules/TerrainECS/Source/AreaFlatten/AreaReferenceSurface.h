#pragma once

#include "TerrainECS/TerrainRoutePolyline.h"
#include "Types/Types.h"

#include <vector>

namespace GameEngine::TerrainECS
{

// A road whose band crosses an area's rim. Where the band covers the rim, the rim takes the road's
// flatten target instead of its own height, blended by the road's shape weight.
struct AreaOpening
{
    // The road's stations near the area, world meters: one polyline per run of consecutive
    // stations, so a road that leaves and re-enters the area keeps its gaps.
    std::vector<RoutePolyline> Runs;
    // The road flatten's target: an offset from the route's height under UseVolumeHeight, else a
    // world Y.
    float32 TargetHeight = 0.0f;
    bool UseVolumeHeight = true;
    // The road volume's shape ramp (ShapeFalloffWeight) and master weight.
    float32 Falloff = 0.0f;
    float32 FalloffInward = 0.0f;
    float32 Weight = 1.0f;
};

// What an area's reference surface is built from. Every field is authored data: the surface never
// reads a baked height, so a second build from the same inputs gives the same surface.
struct AreaSurfaceInputs
{
    // The closed rim, world meters, resampled by arc length; its height is the authored spline's Y.
    RoutePolyline Rim;
    std::vector<AreaOpening> Openings;
    // The world XZ rectangle the surface covers: the area's whole footprint, falloff included.
    float32 MinX = 0.0f;
    float32 MinZ = 0.0f;
    float32 MaxX = 0.0f;
    float32 MaxZ = 0.0f;
    // The surface's sample step, meters.
    float32 Spacing = 0.5f;
};

// The height a flatten on a closed spline area levels toward, as a raster over the area: the harmonic
// (Laplace) fill of the rim inside it, the nearest rim height outside it. Where a road's band crosses
// the rim, the rim takes the road's target across the road's full width.
class AreaReferenceSurface
{
public:
    // The content hash of `inputs`. Two inputs with the same key build the same surface.
    static uint64 HashInputs(const AreaSurfaceInputs& inputs);

    // Rasterizes `inputs` and solves the interior. Deterministic: single-threaded, a fixed order.
    void Build(const AreaSurfaceInputs& inputs);

    // The surface's world Y at a world (x, z): bilinear between samples, clamped to the raster.
    float32 Sample(float32 worldX, float32 worldZ) const;

    // The key of the inputs the surface was built from (HashInputs).
    uint64 GetInputKey() const { return m_InputKey; }

private:
    float32 m_OriginX = 0.0f;
    float32 m_OriginZ = 0.0f;
    float32 m_Spacing = 1.0f;
    uint32 m_Width = 0;
    uint32 m_Height = 0;
    // World Y per sample, row-major from (m_OriginX, m_OriginZ).
    std::vector<float32> m_Heights;
    uint64 m_InputKey = 0;
};

} // namespace GameEngine::TerrainECS
