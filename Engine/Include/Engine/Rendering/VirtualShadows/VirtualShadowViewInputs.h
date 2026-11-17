#pragma once

#include "Mathematics/Matrix4x4.h"
#include "Rendering/Core/Handle.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"

#include <cstdint>

namespace GameEngine::Engine::Renderer
{
/// What one view's lighting passes read to sample virtual shadow pages in this frame.
struct VirtualShadowViewInputs
{
    Rendering::RenderGraph::RGTexture Atlas{};
    Rendering::RenderGraph::RGBuffer PageTable{};
    uint64_t PageTableBytes = 0;
    Rendering::BufferHandle Projections{};
    uint64_t ProjectionsOffset = 0;
    uint64_t ProjectionsBytes = 0;
    Rendering::SamplerHandle DepthSampler{};
    Rendering::SamplerHandle CompareSampler{};
    /// The view's latest sun mask this frame (VirtualShadowSunMask), and the prepass depth and
    /// camera it was resolved from. A world pass reads it only when its depth and camera match.
    Rendering::RenderGraph::RGTexture SunMask{};
    Rendering::RenderGraph::RGTexture SunMaskDepth{};
    Mathematics::Matrix4x4 SunMaskViewProjection{};

    bool IsValid() const
    {
        return Atlas.IsValid() && PageTable.IsValid() && PageTableBytes != 0 && Projections.IsValid() &&
               ProjectionsBytes != 0 && DepthSampler.IsValid() && CompareSampler.IsValid();
    }
};
} // namespace GameEngine::Engine::Renderer
