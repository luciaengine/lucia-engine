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

    /// Records a prepared pipeline in the current overlay pass; an invalid handle draws nothing.
    /// gridColor is packed ARGB; cells2D stores origin X/Y and cell width/height in world units.
    /// A nonpositive cell width selects zoom-based spacing. Raw arrays feed the grid shader layout.
    void RecordRG(const Rendering::RenderGraph::RGContext& context,
                   Rendering::PipelineHandle pipeline,
                   const Rendering::CameraData* camera,
                   const float cameraPosition[3],
                   bool is2DMode,
                   float cameraDistance,
                   float viewportHeight,
                   float gridOpacity,
                   uint32_t gridColor,
                   const float cells2D[4]);
};

} // namespace GameEngine
