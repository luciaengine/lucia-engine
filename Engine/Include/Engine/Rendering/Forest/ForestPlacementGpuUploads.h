#pragma once

#include "Engine/Rendering/Forest/ForestPlacementStream.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/RenderGraph/RGFrameStamp.h"

#include <optional>

namespace GameEngine::Engine::Renderer
{

struct ForestPlacementCopyRun
{
    uint64 StagingOffset;
    uint64 SlotOffset;
    uint64 Bytes;
};

struct ForestPlacementUploadPlan
{
    std::vector<const ForestPlacementUpload*> SlotOrder;
    std::vector<ForestPlacementCopyRun> Runs;
    uint64 Bytes = 0;
};

/// Plans exact byte copies in slot order. The plan borrows the upload records
/// only until the caller packs the staging bytes.
std::optional<ForestPlacementUploadPlan> BuildForestPlacementUploadPlan(
    std::span<const ForestPlacementUpload> uploads, uint32 slotCount);

/// Copies verified placement pages into their device-local slots. The owner
/// keeps the destination and graph stream alive through outstanding submissions.
/// On device rebuild, replace both this uploader and the placement runtime.
class ForestPlacementGpuUploads
{
public:
    ForestPlacementGpuUploads(std::shared_ptr<ForestPlacementRuntime> placements,
                              Rendering::BufferHandle destination, uint32 slots);
    ~ForestPlacementGpuUploads();
    ForestPlacementGpuUploads(const ForestPlacementGpuUploads&) = delete;
    ForestPlacementGpuUploads& operator=(const ForestPlacementGpuUploads&) = delete;

    /// Records a graphics-queue copy from the frame's upload ring. A rejected
    /// batch ends those uploads with failure, so the stand remains visible.
    bool Record(Rendering::RenderGraph::RGFrame& frame, std::span<const ForestPlacementUpload> uploads);

    /// Call once after graph execution or abort. An unsubmitted copy can be
    /// discarded. A submitted copy is retained through its exact queue fence.
    void FinishFrame(Rendering::RenderGraph::RGFrame& frame);

    /// Polls completion without blocking; returns how many tickets published.
    uint32 DrainCompleted(const Rendering::IDevice& device);

private:
    struct Batch;

    std::shared_ptr<ForestPlacementRuntime> m_Placements;
    Rendering::BufferHandle m_Destination;
    uint32 m_SlotCount;
    std::vector<std::shared_ptr<Batch>> m_Batches;
};

} // namespace GameEngine::Engine::Renderer
