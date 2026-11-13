#include "Engine/Rendering/Pipeline/Nodes/VolumetricCloudsParamsUploadNode.h"

#include "Engine/Rendering/FogSunLightingResolver.h"
#include "Engine/Rendering/Pipeline/RenderPipeline.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/VolumetricCloudsParamsUBO.h"

#include <nlohmann/json.hpp>

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{
using namespace ::GameEngine::Rendering;

bool VolumetricCloudsParamsUploadNode::Initialize(std::string nodeId, std::string nodeJson,
                                                  std::string* outError)
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

void VolumetricCloudsParamsUploadNode::DeclareForView(ViewDeclare& d)
{
    const PostProcessSettings& pp =
        d.Services.GetEffectivePostProcessSettings(d.View.id, d.View.worldId);
    // No clouds: no ring allocation and no light resolve. Every consumer of
    // this buffer carries the same gate, so publishing nothing is inert.
    if (!pp.IsVolumetricCloudsActive())
        return;

    // Dissolved into the upload ring: settings resolved + written at
    // declaration; no pass, no graph declaration (host-coherent).
    auto alloc = d.Frame.AllocUpload<VolumetricCloudsParamsUBO>();
    if (!alloc.Valid())
        return;

    VolumetricCloudsParamsUBO ubo{};
    FillVolumetricCloudsParamsUBO(pp, ubo);

    // The cloud sun always tracks the primary directional light, with the sky
    // sun as fallback (ResolveFogSunLighting's chain). The resolved
    // direction points TOWARD the sun — the shader's light-march direction.
    const ResolvedFogSun fallback{Mathematics::Vector3(ubo.sunDirX, ubo.sunDirY, ubo.sunDirZ),
                                  ColorLinear(ubo.sunColorR, ubo.sunColorG, ubo.sunColorB), ubo.sunIntensity};
    const ResolvedFogSun sun = ResolveFogSunLighting(
        d.Services, d.View.id, /*trackDirectionalLight*/ true, /*sunIntensityScale*/ 1.0f, fallback);
    ubo.sunDirX = sun.Direction.x;
    ubo.sunDirY = sun.Direction.y;
    ubo.sunDirZ = sun.Direction.z;
    ubo.sunColorR = sun.Color.r;
    ubo.sunColorG = sun.Color.g;
    ubo.sunColorB = sun.Color.b;
    ubo.sunIntensity = sun.Intensity;

    *alloc.Ptr = ubo;
    d.PublishBuffer(m_BufferRef,
                    {alloc.Buffer, alloc.Offset, sizeof(VolumetricCloudsParamsUBO), /*Graph*/ {}});
}

} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes
