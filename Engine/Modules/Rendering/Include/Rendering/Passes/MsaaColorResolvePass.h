#pragma once

#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Core/RenderGraph/RGFullscreen.h"

namespace GameEngine::Rendering::Passes
{

/// The view's buffers the resolve reads: its ViewParams (the look and the static exposure in ge_exposureParams) and
/// its ExposureHistory (the metered exposure and the pre-exposure). Each is a buffer, offset and size, with Declare
/// valid for a device-local buffer that a pass writes this frame. The pass sets Binding and IsStorage.
struct MsaaResolveViewBuffers
{
    RenderGraph::RGFullscreenBufferInput ViewParams{};
    RenderGraph::RGFullscreenBufferInput ExposureHistory{};
};

/// Declares a thread-safe resolve of multisampled scene color (a ShaderResource texture): each pixel shows the mean
/// display light of its samples under the view's look and exposure. Bad input returns an invalid pass and an error log.
/// @param resolved A single-sample, one-mip, one-layer render target the size of `multisampled`.
/// @param bloomSampleDelta Invalid, or a target like `resolved` that gets the plain sample mean minus the weighted mean.
/// @param view The view's ViewParams and ExposureHistory; both buffers are valid.
RenderGraph::RGPass AddMsaaColorResolvePassRG(RenderGraph::RGFrame& frame, RenderGraph::RGTexture multisampled,
                                              RenderGraph::RGTexture resolved, RenderGraph::RGTexture bloomSampleDelta,
                                              const MsaaResolveViewBuffers& view, const char* passName);

} // namespace GameEngine::Rendering::Passes
