#pragma once

#include "Rendering/Materials/Shader/ShaderVariantKey.h"

namespace GameEngine::Rendering::RenderGraph
{
class RGFrame;
class RGPassBuilder;
} // namespace GameEngine::Rendering::RenderGraph

namespace GameEngine::Engine::Renderer
{
struct ResolvedPassResources;
struct VirtualShadowViewInputs;

/// Set the VirtualShadowMaps keyword only when a shadowed pass can read this frame's inputs.
Rendering::MaterialKeyword ContributeVirtualShadows(const VirtualShadowViewInputs& inputs,
                                                    Rendering::MaterialKeyword keywords);

/// Order a lighting pass after the page table upload and the page raster passes.
void ReadVirtualShadowInputs(Rendering::RenderGraph::RGPassBuilder& pass, const VirtualShadowViewInputs& inputs);

/// Add the four virtual shadow bindings of a pass that has the VirtualShadowMaps keyword.
void BindVirtualShadowInputs(const Rendering::RenderGraph::RGFrame& frame, const VirtualShadowViewInputs& inputs,
                             ResolvedPassResources& resources);
} // namespace GameEngine::Engine::Renderer
