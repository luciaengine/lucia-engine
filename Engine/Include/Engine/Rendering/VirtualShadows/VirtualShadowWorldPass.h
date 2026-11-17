#pragma once

#include "Engine/Rendering/CameraAspectRatio.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Materials/Shader/ShaderVariantKey.h"

namespace GameEngine::Rendering
{
struct CameraData;
} // namespace GameEngine::Rendering

namespace GameEngine::Engine::Renderer
{
class RenderServices;
struct ResolvedPassResources;
struct ResolvedShadowSettings;
struct VirtualShadowViewInputs;

/// What a world pass hands to the sun mask contribution.
struct VirtualShadowSunMaskView
{
    uint32_t ViewId = 0;
    uint64_t WorldId = 0;
    /// The single-sample depth the prepass wrote (the resolved depth of a multisampled view).
    Rendering::RenderGraph::RGTexture Depth{};
    const Rendering::CameraData* Camera = nullptr;
    const ResolvedShadowSettings* Settings = nullptr;
    ViewLetterbox Letterbox{};
    /// The view's prepass wrote the depth and this pass may declare the mask (not the recover pass).
    bool CanDeclare = false;
    bool TransmissiveOnly = false;
};

/// Set the VirtualShadowMaps keyword only when a shadowed pass can read this frame's inputs.
/// Clears VirtualShadowSunMask: only ContributeVirtualShadowSunMask sets it.
Rendering::MaterialKeyword ContributeVirtualShadows(const VirtualShadowViewInputs& inputs,
                                                    Rendering::MaterialKeyword keywords);

/// Set the VirtualShadowSunMask keyword on an opaque world pass whose sun reads its
/// virtual page visibility from the sun mask. Declares the mask pass once per view
/// and frame, after the prepass. Call after ContributeVirtualShadows.
Rendering::MaterialKeyword ContributeVirtualShadowSunMask(RenderServices& services,
                                                          Rendering::RenderGraph::RGFrame& frame,
                                                          const VirtualShadowSunMaskView& view,
                                                          VirtualShadowViewInputs& inputs,
                                                          Rendering::MaterialKeyword keywords);

/// Order a lighting pass after the page table upload, the page raster passes and,
/// with the VirtualShadowSunMask keyword, the sun mask pass.
void ReadVirtualShadowInputs(Rendering::RenderGraph::RGPassBuilder& pass, const VirtualShadowViewInputs& inputs,
                             Rendering::MaterialKeyword keywords);

/// Add the virtual shadow bindings of a pass that has the VirtualShadowMaps keyword.
void BindVirtualShadowInputs(const Rendering::RenderGraph::RGFrame& frame, const VirtualShadowViewInputs& inputs,
                             Rendering::MaterialKeyword keywords, ResolvedPassResources& resources);
} // namespace GameEngine::Engine::Renderer
