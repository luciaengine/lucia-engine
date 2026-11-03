#pragma once

#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"

#include <string>

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{
// The render->output crossing for a smooth pixel-perfect 2D camera.
//
// The camera rasterizes into a padded reference RT (reference + a one-texel
// border) and is shown at an integer zoom, centered, with the rest of the panel
// letterboxed black. Doing that blit AFTER the pipeline would leave the whole
// post chain at the art resolution, where a texel-scale display effect has
// roughly one sample per art texel: a CRT's intra-scanline profile collapses to
// one darkening value per texel and the nearest zoom tiles it into a uniform
// screen-door mesh. Placing the blit INSIDE the pipeline, at the boundary the
// blueprint's extent.basis already draws, puts bloom, tonemapping and the LDR
// display effects (CRT, VHS, film grain) at the panel extent over an
// integer-zoomed image — the resolution those effects are written for.
//
// Two things happen here, matching RenderScaleUpscaleNode:
//   * colour crosses from the padded reference extent to the display extent
//     (nearest, sub-texel window offset, centered rect, black bars — the
//     mapping is contract, stated on PixelPerfectUpscaleParamsRG);
//   * View.Resolve is republished to the pipeline's display-extent colour, so
//     the terminal stage writes the panel-sized target rather than the padded
//     reference one.
// No depth is reconstituted: nothing past the crossing depth-tests at the
// display extent, and the absent View.OutputDepth is exactly what keeps the
// render-scale crossing (which requires both) off a pixel-perfect view.
//
// Inert unless the view carries an active pixel-perfect crossing: the input is
// stitched through under the output name and no pass is declared.
//
// JSON shape: { "id": "PixelPerfectUpscale", "type": "PixelPerfectUpscale",
//               "input": "HDRUpscaled", "output": "HDRPixelPerfect" }
class PixelPerfectUpscaleNode final : public IRenderPipelineNode
{
  public:
    const char* GetTypeName() const override { return "PixelPerfectUpscale"; }

    bool Initialize(std::string nodeId, std::string nodeJson, std::string* outError) override;
    void DeclareForView(ViewDeclare& d) override;

  private:
    std::string m_InputKey = "HDRUpscaled";
    std::string m_OutputKey = "HDRPixelPerfect";
    std::string m_Id;

    bool m_WarnedOutputBasis = false;
    bool m_WarnedSourceExtent = false;
};

} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes
