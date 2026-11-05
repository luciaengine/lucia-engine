#pragma once

#include "Rendering/Core/Handle.h"

#include <cstdint>

namespace GameEngine {

namespace Rendering {
struct CameraData;
namespace RenderGraph {
class RGContext;
class RGPrepareContext;
}
} // namespace Rendering

// Renders the infinite editor grid for both 3D (Y=0 plane) and 2D (Z=0 / XY
// plane) scene view modes. Owns the lazily-created GPU pipelines and shader
// loading. Stateless: caller passes grid opacity, camera data, etc. per frame.
class SceneViewGridRenderer
{
public:
    /// Prepares the grid pipeline on the caller thread before overlay recording.
    Rendering::PipelineHandle PrepareRG(const Rendering::RenderGraph::RGPrepareContext& context,
                                         bool is2DMode);

    // Record grid draw commands into the current gizmo overlay pass.
    // Call exactly once per frame, inside the gizmo overlay execute lambda.
    // gridColor is ARGB packed; only the rgb channels reach the shader (the
    // axis lines keep their hardcoded conventional colors).
    // cells2D = {originX, originY, cellWidth, cellHeight} in world units; a
    // non-positive width makes the 2D grid fall back to zoom-based decades.
    //
    void RecordRG(const Rendering::RenderGraph::RGContext& ctx,
                   Rendering::PipelineHandle pipeline,
                   const Rendering::CameraData* cam,
                   const float camPos[3],
                   bool is2DMode,
                   float camDistance,
                   float viewportH,
                   float gridOpacity,
                   uint32_t gridColor,
                   const float cells2D[4]);
};

} // namespace GameEngine
