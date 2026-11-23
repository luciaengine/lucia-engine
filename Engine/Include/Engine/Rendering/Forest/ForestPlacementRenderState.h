#pragma once

#include "Engine/Rendering/Forest/ForestPlacementGpuUploads.h"

#include <deque>

namespace GameEngine::Engine::Renderer
{

/// A primary view's placement stream and graphics-buffer residency. Prepare is
/// called once per graph incarnation. Use its detail draw ranges only on the
/// graphics queue, then call Finish after that graph submits or aborts.
class ForestPlacementRenderState
{
public:
    ForestPlacementRenderState(std::shared_ptr<ForestPlacementRuntime> placements,
                               Rendering::BufferHandle destination, uint32 slots);

    /// Calculates a view plan and records pending uploads. The buffer and graph
    /// stream must outlive pending reads and uploads, including their fences.
    bool Prepare(AssetManager& assets, Rendering::IDevice& device, Rendering::RenderGraph::RGFrame& graph,
                 const ForestPlacementView& view, ForestPlacementFrame& plan);
    void Finish(Rendering::RenderGraph::RGFrame& graph, bool submitted);

private:
    struct DrawFence
    {
        uint64 Frame;
        Rendering::IDevice::GpuSyncToken Token;
    };

    ForestPlacementStream m_Stream;
    ForestPlacementGpuUploads m_Uploader;
    std::vector<ForestPlacementUpload> m_Uploads;
    std::deque<DrawFence> m_ReadFences;
    Rendering::RenderGraph::RGFrameStamp m_Prepared;
    uint64 m_LastFinished = 0;
    bool m_DeclaredDetail = false;
};

} // namespace GameEngine::Engine::Renderer
