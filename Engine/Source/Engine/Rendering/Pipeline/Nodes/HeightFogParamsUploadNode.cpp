#include "Engine/Rendering/Pipeline/Nodes/HeightFogParamsUploadNode.h"

#include "Engine/Rendering/FogSunLightingResolver.h"
#include "Engine/Rendering/HeightFogParamsUBO.h"
#include "Engine/Rendering/Pipeline/RenderPipeline.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/SkyRenderFeature.h"

#include <nlohmann/json.hpp>

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{
using namespace ::GameEngine::Rendering;

bool HeightFogParamsUploadNode::Initialize(std::string nodeId, std::string nodeJson, std::string* outError)
{
    m_Id = std::move(nodeId);
    m_Json = std::move(nodeJson);

    try
    {
        auto j = nlohmann::json::parse(m_Json);
        if (j.is_object() && j.contains("buffer") && j["buffer"].is_string())
            m_BufferRef = j["buffer"].get<std::string>();
    }
    catch (const std::exception& e)
    {
        if (outError)
            *outError = std::string("JSON parse failed: ") + e.what();
        return false;
    }

    return true;
}

void HeightFogParamsUploadNode::DeclareForView(ViewDeclare& d)
{
    // Dissolved into the upload ring: settings resolved + written at
    // declaration; no pass, no graph declaration (host-coherent).
    auto alloc = d.Frame.AllocUpload<HeightFogParamsUBO>();
    if (!alloc.Valid())
        return;

    const PostProcessSettings& pp =
        d.Services.GetEffectivePostProcessSettings(d.View.id, d.View.worldId);
    HeightFogParamsUBO ubo{};
    FillHeightFogParamsUBO(pp, ubo);
    ubo.noiseTime = d.Services.GetShaderAnimationTimeSeconds();

    // The same "is there a sky" the SkyRender node draws by: without it there is no backdrop.
    const SkyRenderFeature* sky = d.Services.GetFeature<SkyRenderFeature>();
    const bool skyActive = sky && sky->IsInitialized() && sky->HasActiveSettings();
    FillHeightFogSkyColor(pp, skyActive ? &sky->GetSettings() : nullptr, ubo);

    if (pp.HeightFogTrackDirectionalLight)
    {
        const ResolvedFogSun fallback{
            Mathematics::Vector3(pp.HeightFogSunDirX, pp.HeightFogSunDirY, pp.HeightFogSunDirZ),
            ColorLinear(pp.HeightFogSunColorR, pp.HeightFogSunColorG, pp.HeightFogSunColorB),
            pp.HeightFogSunIntensity};
        const ResolvedFogSun sun = ResolveFogSunLighting(
            d.Services, d.View.id, pp.HeightFogTrackDirectionalLight != 0, pp.HeightFogSunIntensityScale, fallback);
        ubo.sunDirX = sun.Direction.x;
        ubo.sunDirY = sun.Direction.y;
        ubo.sunDirZ = sun.Direction.z;
        ubo.sunColorR = sun.Color.r;
        ubo.sunColorG = sun.Color.g;
        ubo.sunColorB = sun.Color.b;
        ubo.sunIntensity = sun.Intensity;
    }

    *alloc.Ptr = ubo;
    d.PublishBuffer(m_BufferRef,
                    {alloc.Buffer, alloc.Offset, sizeof(HeightFogParamsUBO), /*Graph*/ {}});
}

} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes
