#include "Engine/Rendering/Pipeline/Nodes/PixelPerfectUpscaleNode.h"

#include "Engine/Rendering/Pipeline/PipelineResourceNames.h"
#include "Engine/Rendering/RenderServices.h"

#include "Logger/Logger.h"
#include "Rendering/Passes/PixelPerfectUpscalePass.h"

#include <nlohmann/json.hpp>

#include <utility>

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{
using namespace ::GameEngine::Rendering;

bool PixelPerfectUpscaleNode::Initialize(std::string nodeId, std::string nodeJson,
                                         std::string* /*outError*/)
{
    m_Id = std::move(nodeId);
    try
    {
        auto j = nlohmann::json::parse(nodeJson);
        if (j.is_object())
        {
            if (j.contains("input") && j["input"].is_string())
                m_InputKey = j["input"].get<std::string>();
            if (j.contains("output") && j["output"].is_string())
                m_OutputKey = j["output"].get<std::string>();
        }
    }
    catch (const std::exception&)
    {
        // Keep defaults on malformed config.
    }
    return true;
}

void PixelPerfectUpscaleNode::DeclareForView(ViewDeclare& d)
{
    const RenderGraph::RGTexture input = d.ResolveTexture(m_InputKey);
    // Stitch-through: the output name must resolve for downstream consumers
    // whether the crossing runs or not, and NEVER before the output ref is
    // resolved — publishing first would shadow the blueprint resource under its
    // own name, so the output resolve would hand back the render-extent input.
    auto stitchThrough = [&]()
    {
        if (input.IsValid())
            d.PublishTexture(m_OutputKey, input);
    };

    // The crossing is active exactly when the pre-pass sized Output* from the
    // view's pixel-perfect state and materialized the display-extent colour.
    if (!d.PixelPerfectCrossing || !input.IsValid() || !d.ViewOutputColor.IsValid() ||
        d.OutputWidth == 0 || d.OutputHeight == 0)
    {
        stitchThrough();
        return;
    }

    const Engine::Renderer::PixelPerfectViewState pp =
        d.Services.Views().GetViewPixelPerfect(d.View.id);
    if (!pp.Active || pp.Zoom == 0 || pp.PaddedWidth == 0 || pp.PaddedHeight == 0)
    {
        stitchThrough();
        return;
    }

    // The window mapping is stated in source texels of the padded RT, so an
    // input that is not that RT would sample the wrong grid. A mismatch means
    // something upstream already resampled — decline rather than blit a lie.
    const auto& inDesc = d.Frame.Graph().ResourceDesc(input.Id);
    if (inDesc.Width != pp.PaddedWidth || inDesc.Height != pp.PaddedHeight)
    {
        if (!m_WarnedSourceExtent)
        {
            m_WarnedSourceExtent = true;
            Logger::Log::Warning(
                "PixelPerfectUpscaleNode '{}': input '{}' is {}x{} but the camera's padded "
                "reference RT is {}x{} — crossing skipped",
                m_Id, m_InputKey, inDesc.Width, inDesc.Height, pp.PaddedWidth, pp.PaddedHeight);
        }
        stitchThrough();
        return;
    }

    // The output resolves through the blueprint, whose declaration carries
    // extent.basis "output" — that is what places this node on the display side
    // of the boundary. A render-basis output would make the node a no-op copy.
    const RenderGraph::RGTexture outColor = d.ResolveTexture(m_OutputKey);
    if (!outColor.IsValid())
    {
        stitchThrough();
        return;
    }
    const auto& outDesc = d.Frame.Graph().ResourceDesc(outColor.Id);
    if (outDesc.Width != d.OutputWidth || outDesc.Height != d.OutputHeight)
    {
        if (!m_WarnedOutputBasis)
        {
            m_WarnedOutputBasis = true;
            Logger::Log::Warning(
                "PixelPerfectUpscaleNode '{}': output '{}' materialized {}x{} but the view's "
                "display extent is {}x{} — the blueprint resource needs extent.basis \"output\"; "
                "crossing skipped",
                m_Id, m_OutputKey, outDesc.Width, outDesc.Height, d.OutputWidth, d.OutputHeight);
        }
        stitchThrough();
        return;
    }

    Passes::PixelPerfectUpscaleParamsRG params{};
    params.Src = input;
    params.Dst = outColor;
    // Mid-chain: the HDR chain is display-linear here and the terminal encode
    // downstream owns the transfer function.
    params.PassthroughLinear = true;
    params.ReferenceWidth = pp.ReferenceWidth;
    params.ReferenceHeight = pp.ReferenceHeight;
    params.SourceWidth = pp.PaddedWidth;
    params.SourceHeight = pp.PaddedHeight;
    params.Zoom = pp.Zoom;
    params.FracX = pp.FracX;
    params.FracY = pp.FracY;
    if (!Passes::AddPixelPerfectUpscalePassRG(d.Frame, params, Passes::FinalizeInputSpace::Linear,
                                              d.PassName("PixelPerfectUpscale").c_str())
             .IsValid())
    {
        stitchThrough();
        return;
    }

    d.PublishTexture(m_OutputKey, outColor);

    // Redirect the final-output slot to the display-extent colour: View.Resolve
    // served the world half as the padded reference target up to here; from the
    // crossing on (bloom -> ... -> FinalCopy, and the blueprint's
    // outputs.FinalColor) it must be the panel-sized one, or the terminal stage
    // would resample the finished image back DOWN into the reference RT.
    d.PublishTexture(Names::View::Resolve, d.ViewOutputColor);
}

} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes
