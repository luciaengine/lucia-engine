#include "Engine/Rendering/VirtualShadows/VirtualShadowWorldPass.h"

#include "Engine/Rendering/PassBindingContext.h"
#include "Engine/Rendering/VirtualShadows/VirtualShadowViewInputs.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Types/StringId.h"

namespace GameEngine::Engine::Renderer
{
namespace RenderGraph = Rendering::RenderGraph;
using Rendering::MaterialKeyword;

MaterialKeyword ContributeVirtualShadows(const VirtualShadowViewInputs& inputs, MaterialKeyword keywords)
{
    keywords = keywords & ~MaterialKeyword::VirtualShadowMaps;
    if (!inputs.IsValid() || !Rendering::HasKeyword(keywords, MaterialKeyword::Shadows))
        return keywords;
    return keywords | MaterialKeyword::VirtualShadowMaps;
}

void ReadVirtualShadowInputs(RenderGraph::RGPassBuilder& pass, const VirtualShadowViewInputs& inputs)
{
    if (!inputs.IsValid())
        return;
    pass.Read(inputs.Atlas, RenderGraph::RGTextureRead::Sampled);
    pass.Read(inputs.PageTable, RenderGraph::RGBufferRead::Storage);
}

void BindVirtualShadowInputs(const RenderGraph::RGFrame& frame, const VirtualShadowViewInputs& inputs,
                             ResolvedPassResources& resources)
{
    const auto atlas = frame.PhysicalTexture(inputs.Atlas);
    const auto table = frame.PhysicalBuffer(inputs.PageTable);
    if (!inputs.IsValid() || !atlas.IsValid() || !table.IsValid())
        return;
    resources.Buffers.push_back({HashStringId("VirtualShadowPageTable"), table, 0, inputs.PageTableBytes});
    resources.Buffers.push_back({HashStringId("VirtualShadowProjections"), inputs.Projections,
                                 inputs.ProjectionsOffset, inputs.ProjectionsBytes});
    resources.Textures.push_back({HashStringId("ge_virtualShadowDepth"), atlas, inputs.DepthSampler});
    resources.Textures.push_back({HashStringId("ge_virtualShadowDepthCompare"), atlas, inputs.CompareSampler});
}
} // namespace GameEngine::Engine::Renderer
