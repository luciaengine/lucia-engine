#pragma once

#include "Rendering/Core/RenderGraph/RGFrame.h"

#include <optional>
#include <span>
#include <string_view>

namespace GameEngine::Engine::Renderer
{

struct ForestPlacementGpuPage;
struct ForestPlacementGpuView;

/// Compacts the primary view's resident placements on the graphics queue.
class ForestPlacementCull
{
public:
    /// Load the packaged kernel for this device and request its pipeline.
    bool Initialize(Rendering::IDevice& device);

    struct DrawBuffers
    {
        Rendering::RenderGraph::RGBuffer Slots;
        Rendering::RenderGraph::RGBuffer Visible;
        Rendering::RenderGraph::RGBuffer Args;
        Rendering::RenderGraph::RGBuffer Pages;
        Rendering::RenderGraph::RGBuffer View;
        uint64 SlotBytes;
        uint64 VisibleBytes;
        uint64 PagesOffset;
        uint64 PagesBytes;
        uint64 ViewOffset;
    };

    /// The owner and slot buffer must outlive graph execution. The draw pass
    /// reads Args as indirect, Slots/Visible/Pages as storage and View as uniform.
    [[nodiscard]] std::optional<DrawBuffers> Declare(Rendering::RenderGraph::RGFrame& frame,
        std::string_view scope, Rendering::BufferHandle slots, uint32 slotCount, const ForestPlacementGpuView& view,
        std::span<const ForestPlacementGpuPage> pages);

private:
    uint32 m_VisiblePageCapacity = 0;
    Rendering::ComputePipelineId m_Pipeline;
    Rendering::DescriptorSetLayoutDesc m_PlacementsLayout;
    Rendering::DescriptorSetLayoutDesc m_ViewLayout;
};

} // namespace GameEngine::Engine::Renderer
