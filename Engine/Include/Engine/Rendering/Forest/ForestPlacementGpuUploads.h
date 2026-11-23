#pragma once

#include "Engine/Rendering/Forest/ForestPlacementStream.h"
#include "Rendering/Core/Handle.h"

#include <memory>
#include <span>
#include <vector>

namespace GameEngine::Rendering
{
class IDevice;
namespace RenderGraph
{
class RGFrame;
}
} // namespace GameEngine::Rendering

namespace GameEngine::Engine::Renderer
{

/// Copies verified placement pages into their device-local slots on the graphics queue. The owner
/// keeps the destination and the graph stream alive through outstanding submissions. On a device
/// rebuild the owner destroys the uploader with its ForestPlacementRenderState, which resets the
/// runtime's residency. Runs on the render thread.
class ForestPlacementGpuUploads
{
public:
    ForestPlacementGpuUploads(std::shared_ptr<ForestPlacementRuntime> placements,
                              Rendering::BufferHandle destination, uint32 slots);
    ~ForestPlacementGpuUploads();
    ForestPlacementGpuUploads(const ForestPlacementGpuUploads&) = delete;
    ForestPlacementGpuUploads& operator=(const ForestPlacementGpuUploads&) = delete;

    /// Records a graphics-queue copy from the frame's upload ring. A rejected batch fails those uploads,
    /// so their pages keep drawing the overview.
    bool Record(Rendering::RenderGraph::RGFrame& frame, std::span<const ForestPlacementUpload> uploads);

    /// Call once after the graph submits or aborts. An unsubmitted copy is abandoned. A submitted copy
    /// waits for its queue fence.
    void FinishFrame(Rendering::RenderGraph::RGFrame& frame);

    /// Polls the fences without blocking and reports finished copies. Returns how many slots publish.
    uint32 DrainCompleted(const Rendering::IDevice& device);

private:
    struct Batch;
    bool HasOutstandingCopy(ForestPageTicket ticket) const;
    // Fails each upload that has no outstanding copy. Always returns false.
    bool RejectUploads(std::span<const ForestPlacementUpload> uploads);

    std::shared_ptr<ForestPlacementRuntime> m_Placements;
    Rendering::BufferHandle m_Destination;
    uint32 m_SlotCount;
    std::vector<std::shared_ptr<Batch>> m_Batches;
};

} // namespace GameEngine::Engine::Renderer
