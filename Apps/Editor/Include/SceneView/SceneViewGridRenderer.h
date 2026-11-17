#pragma once

#include "Rendering/Core/Handle.h"

#include <cstdint>
#include <vector>

namespace GameEngine {

namespace Mathematics {
struct Vector3;
struct Vector4;
} // namespace Mathematics

namespace Rendering {
struct CameraData;
struct GraphicsPipelineDesc;
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
    /// The grid pipeline of a mode: the given shaders, depth-tested blending, and a
    /// push constant range sized to the mode's constants (128 bytes in 2D, 112 in 3D).
    static Rendering::GraphicsPipelineDesc PipelineDesc(bool is2DMode, const std::vector<uint8_t>& vertexShader,
                                                        const std::vector<uint8_t>& fragmentShader);

    /// Prepares the grid pipeline on the caller thread before overlay recording.
    Rendering::PipelineHandle PrepareRG(const Rendering::RenderGraph::RGPrepareContext& context,
                                         bool is2DMode);

    // Record grid draw commands into the current gizmo overlay pass.
    // Call exactly once per frame, inside the gizmo overlay execute lambda.
    // gridColor is ARGB packed; only the rgb channels reach the shader (the
    // axis lines keep their hardcoded conventional colors). cells2D is the 2D
    // mode's authored cell grid in world units: origin x, origin y, cell width,
    // cell height; a width <= 0 keeps the zoom-based decades. An invalid
    // pipeline draws nothing.
    void RecordRG(const Rendering::RenderGraph::RGContext& ctx,
                   Rendering::PipelineHandle pipeline,
                   const Rendering::CameraData* cam,
                   const Mathematics::Vector3& camPos,
                   bool is2DMode,
                   float camDistance,
                   float viewportH,
                   float gridOpacity,
                   uint32_t gridColor,
                   const Mathematics::Vector4& cells2D);
};

} // namespace GameEngine
