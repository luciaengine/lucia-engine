#pragma once

#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"
#include "Rendering/Materials/ShaderVariantKey.h"

#include <string>

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{
// Draws the sorted artwork collected by WorldRenderNode into the same targets.
//
// JSON shape (place AFTER the ScatterB / Transmissive entries):
// {
//   "id": "SortedTransparent",
//   "type": "SortedTransparent",
//   "enabled": true,
//   "keywords": ["ForwardPlus", "Instanced", "Shadows", "IBL"]
// }
// The "keywords" array MUST stay in sync with the WorldRender node's: the drain
// selects the material color variant from these keywords, so a mismatch compiles
// a different variant than the opaque pass and the transparents shade
// inconsistently.
class SortedTransparentNode final : public IRenderPipelineNode
{
  public:
    const char* GetTypeName() const override { return "SortedTransparent"; }

    bool Initialize(std::string nodeId, std::string nodeJson, std::string* outError) override;
    void DeclareForView(ViewDeclare& d) override;

  private:
    std::string m_Id;
    std::string m_Json;
    Rendering::MaterialKeyword m_PassKeywords = Rendering::MaterialKeyword::None;
};

} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes
