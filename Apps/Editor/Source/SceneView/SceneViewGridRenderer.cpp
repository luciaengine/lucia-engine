#include "SceneView/SceneViewGridRenderer.h"

#include "Mathematics/MatrixOps.h"
#include "Rendering/Camera/CameraTypes.h"
#include "Rendering/Common/Utils.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Pipeline/PipelineTypes.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Core/ReverseZ.h"

#include <cstring>
#include <vector>

namespace GameEngine {

using namespace Rendering;

// Push constant structs. GridPC is 112 bytes (mat4 64 + 2 vec4 32 + 4 floats 16);
// Grid2DPC adds the 2D cell grid vec4 (128 bytes). Each mode's pipeline declares
// the range of its own struct (SceneViewGridRenderer::PipelineDesc).

struct GridPC
{
    float invVP[16];
    float cameraPos[4];
    float gridColor[4]; // rgb = non-axis line color, a unused
    float opacity;
    float fadeStart;
    float fadeEnd;
    float pad;
};

struct Grid2DPC
{
    float invVP[16];
    float cameraPos[4]; // unused by 2D shader; kept for layout parity with GridPC
    float gridColor[4]; // rgb = non-axis line color, a unused
    float opacity;
    float camDistance;
    float viewportH;
    float pad;
    float cells[4]; // xy = cell origin, zw = cell size; z <= 0 = no authored cells
};

static_assert(sizeof(GridPC) == 112);
static_assert(sizeof(Grid2DPC) == 128);

// ---------------------------------------------------------------------------
// Shader loading (cached, loaded once per process)
// ---------------------------------------------------------------------------

static bool LoadGridShaderBytes(std::vector<uint8_t>& outVs, std::vector<uint8_t>& outFs)
{
    static std::vector<uint8_t> sVs;
    static std::vector<uint8_t> sFs;
    static bool sTried = false;

    if (!sTried)
    {
        sTried = true;
        sVs = Utils::LoadShaderFile("Shaders/infinite_grid.vert.spv");
        sFs = Utils::LoadShaderFile("Shaders/infinite_grid.frag.spv");
    }

    outVs = sVs;
    outFs = sFs;
    return !outVs.empty() && !outFs.empty();
}

static bool LoadGrid2DShaderBytes(std::vector<uint8_t>& outVs, std::vector<uint8_t>& outFs)
{
    static std::vector<uint8_t> sVs;
    static std::vector<uint8_t> sFs;
    static bool sTried = false;

    if (!sTried)
    {
        sTried = true;
        sVs = Utils::LoadShaderFile("Shaders/infinite_grid.vert.spv");
        sFs = Utils::LoadShaderFile("Shaders/infinite_grid_2d.frag.spv");
    }

    outVs = sVs;
    outFs = sFs;
    return !outVs.empty() && !outFs.empty();
}

// ---------------------------------------------------------------------------
// Pipeline creation (shared by both modes)
// ---------------------------------------------------------------------------

GraphicsPipelineDesc SceneViewGridRenderer::PipelineDesc(bool is2DMode, const std::vector<uint8_t>& vs,
                                                         const std::vector<uint8_t>& fs)
{
    GraphicsPipelineDesc gd{};
    gd.Kind = GraphicsPipelineKind::VertexFragment;
    gd.VertexShader = std::make_shared<const std::vector<uint8_t>>(vs);
    gd.PixelShader  = std::make_shared<const std::vector<uint8_t>>(fs);
    gd.DebugName = is2DMode ? "SV_InfiniteGrid2D" : "SV_InfiniteGrid";
    gd.Topology  = PrimitiveTopology::TriangleList;

    DynamicStateInfo dyn{};
    dyn.states.push_back(DynamicState::Viewport);
    dyn.states.push_back(DynamicState::Scissor);
    gd.DynamicState = std::move(dyn);

    gd.Rasterization.cullMode = CullModeFlagBits::None;
    // Depth test ON so the grid is occluded by scene geometry. Depth write
    // OFF so the grid doesn't occlude gizmos drawn after it.
    gd.DepthStencil.depthTestEnable  = true;
    gd.DepthStencil.depthWriteEnable = false;
    gd.DepthStencil.depthCompareOp   = ReverseZ::kNearerOrEqual;

    ColorBlendAttachmentState blend{};
    blend.blendEnable = true;
    blend.srcColorBlendFactor = BlendFactor::One;
    blend.dstColorBlendFactor = BlendFactor::OneMinusSrcAlpha;
    blend.srcAlphaBlendFactor = BlendFactor::One;
    blend.dstAlphaBlendFactor = BlendFactor::OneMinusSrcAlpha;
    gd.ColorBlend.attachments = {blend};

    gd.PushConstants.Size      = static_cast<uint32_t>(is2DMode ? sizeof(Grid2DPC) : sizeof(GridPC));
    gd.PushConstants.StageMask = kShaderStageFragment;

    return gd;
}

// Shared per-frame inputs → push constants. Both PC structs differ only in the
// last two scalars, so one filler covers the common prefix.
static void FillGridPCCommon(float invVPOut[16], float cameraPosOut[4], float gridColorOut[4],
                             float& opacityOut, const Rendering::CameraData& cam,
                             const float camPos[3], float gridOpacity, uint32_t gridColor)
{
    Mathematics::Matrix4x4 invVP = Mathematics::Inverse(cam.viewProj);
    std::memcpy(invVPOut, invVP.Data(), sizeof(float) * 16);

    cameraPosOut[0] = camPos[0];
    cameraPosOut[1] = camPos[1];
    cameraPosOut[2] = camPos[2];
    cameraPosOut[3] = 0.0f;

    // Unpack ARGB → linear floats. Alpha drives the grid's opacity multiplier
    // so the configured grid color controls visibility directly; the legacy
    // gridOpacity slider stays in the chain as a master multiplier on top.
    gridColorOut[0] = static_cast<float>((gridColor >> 16) & 0xFFu) / 255.0f;
    gridColorOut[1] = static_cast<float>((gridColor >>  8) & 0xFFu) / 255.0f;
    gridColorOut[2] = static_cast<float>((gridColor      ) & 0xFFu) / 255.0f;
    gridColorOut[3] = 1.0f;
    const float gcA = static_cast<float>((gridColor >> 24) & 0xFFu) / 255.0f;
    opacityOut = gridOpacity * gcA;
}

// ---------------------------------------------------------------------------
// Record
// ---------------------------------------------------------------------------

PipelineHandle SceneViewGridRenderer::PrepareRG(const RenderGraph::RGPrepareContext& context,
                                                  bool is2DMode)
{
    IDevice* device = context.GetDevice();
    if (device == nullptr)
        return {};
    std::vector<uint8_t> vertexShader;
    std::vector<uint8_t> fragmentShader;
    const bool loaded = is2DMode ? LoadGrid2DShaderBytes(vertexShader, fragmentShader)
                                 : LoadGridShaderBytes(vertexShader, fragmentShader);
    if (!loaded)
        return {};
    const GraphicsPipelineId pipeline =
        device->InternGraphicsPipeline(PipelineDesc(is2DMode, vertexShader, fragmentShader));
    return context.PreparePipeline(pipeline);
}

void SceneViewGridRenderer::RecordRG(const Rendering::RenderGraph::RGContext& ctx,
                                      Rendering::PipelineHandle pipeline,
                                      const Rendering::CameraData* cam,
                                      const float camPos[3],
                                      bool is2DMode,
                                      float camDistance,
                                      float viewportH,
                                      float gridOpacity,
                                      uint32_t gridColor,
                                      const float cells2D[4])
{
    if (!cam)
        return;

    CommandList* cl = ctx.Cmd;
    if (!cl || !pipeline.IsValid())
        return;

    cl->SetPipeline(pipeline);

    if (!is2DMode)
    {
        GridPC pc{};
        FillGridPCCommon(pc.invVP, pc.cameraPos, pc.gridColor, pc.opacity, *cam, camPos,
                         gridOpacity, gridColor);
        pc.fadeStart = 80.0f;
        pc.fadeEnd = 200.0f;
        cl->SetPushConstants(pc);
    }
    else
    {
        Grid2DPC pc{};
        FillGridPCCommon(pc.invVP, pc.cameraPos, pc.gridColor, pc.opacity, *cam, camPos,
                         gridOpacity, gridColor);
        pc.camDistance = camDistance;
        pc.viewportH = viewportH;
        for (int i = 0; i < 4; ++i)
            pc.cells[i] = cells2D[i];
        cl->SetPushConstants(pc);
    }

    cl->Draw(3u, 1u);
}

} // namespace GameEngine
