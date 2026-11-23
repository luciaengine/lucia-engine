#pragma once

#include "Engine/Rendering/Forest/ForestPlacementGpuUploads.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/RenderGraph/RGFrameStamp.h"

#include <deque>

namespace GameEngine::Engine::Renderer
{

/// A primary view's placement stream, slot uploads and reader fences. Call Prepare once per graph,
/// draw its detail ranges only on the graphics queue, then call Finish after that graph submits or
/// aborts. A slot drawn in a frame is reused only after that frame's fence completes. The destructor
/// resets the runtime's residency, because the slots' uploads and fences end with it. Runs on the
/// render thread.
class ForestPlacementRenderState
{
public:
    ForestPlacementRenderState(std::shared_ptr<ForestPlacementRuntime> placements,
                               Rendering::BufferHandle destination, uint32 slots);
    ~ForestPlacementRenderState();
    ForestPlacementRenderState(const ForestPlacementRenderState&) = delete;
    ForestPlacementRenderState& operator=(const ForestPlacementRenderState&) = delete;

    /// Calculates a view plan and records pending uploads. The buffer and graph
    /// stream must outlive pending reads and uploads, including their fences.
    bool Prepare(AssetManager& assets, Rendering::IDevice& device, Rendering::RenderGraph::RGFrame& graph,
                 const ForestPlacementView& view, ForestPlacementFrame& plan);
    /// Ends the frame of `graph`: a submitted frame fences its detail draws, an aborted one abandons
    /// its uploads.
    void Finish(Rendering::RenderGraph::RGFrame& graph, bool submitted);

private:
    struct DrawFence
    {
        uint64 Frame;
        Rendering::IDevice::GpuSyncToken Token;
    };

    std::shared_ptr<ForestPlacementRuntime> m_Placements;
    ForestPlacementStream m_Stream;
    ForestPlacementGpuUploads m_Uploader;
    std::vector<ForestPlacementUpload> m_Uploads;
    std::deque<DrawFence> m_ReadFences;
    Rendering::RenderGraph::RGFrameStamp m_Prepared;
    uint64 m_LastFinished = 0;
    bool m_DeclaredDetail = false;
};

} // namespace GameEngine::Engine::Renderer
