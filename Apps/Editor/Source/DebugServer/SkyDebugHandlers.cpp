#include "DebugServer/SkyDebugHandlers.h"

#include "Core/Engine.h"
#include "DebugServer/DebugServerReply.h"
#include "DebugServer/EditorDebugServer.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/SkyRenderFeature.h"
#include "Mathematics/Vector3.h"
#include "Rendering/Sky/SkySettings.h"
#include "Types/Color.h"

#include <nlohmann/json.hpp>

namespace GameEngine
{

using json = nlohmann::json;

namespace
{

json Vector3(const Mathematics::Vector3& v)
{
    return json::array({v.x, v.y, v.z});
}

json Rgb(const ColorLinear& c)
{
    return json::array({c.r, c.g, c.b});
}

// Every float is reported as a double converted from the float, which is exact, so equal JSON means
// equal bits (apart from the sign of zero, which JSON keeps as -0).
json DescribeSkySettings(const Rendering::SkySettings& s)
{
    return json{
        {"timeOfDayHours", s.timeOfDayHours},
        {"exposureEV", s.exposureEV},
        {"scatteringSunDir", Vector3(s.scatteringSunDir)},
        {"primarySunDir", Vector3(s.primarySunDir)},
        {"primarySunColor", Rgb(s.primarySunColor)},
        {"primarySunGroundColor", Rgb(s.primarySunGroundColor)},
        {"primarySunIntensity", s.primarySunIntensity},
        {"showSunDisk", s.showSunDisk},
        {"sunSize", s.sunSize},
        {"moonDirWS", Vector3(s.moonDirWS)},
        {"moonIntensity", s.moonIntensity},
        {"moonAngularRadius", s.moonAngularRadius},
        {"showMoonDisk", s.showMoonDisk},
        {"moonExposureEV", s.moonExposureEV},
        {"moonArcPosition", s.moonArcPosition},
        {"fallingStarsEnabled", s.fallingStarsEnabled},
        {"nightSkyBlend", s.nightSkyBlend},
        {"skyTimeSeconds", s.skyTimeSeconds},
        {"starLimitingMagnitude", s.starLimitingMagnitude},
        {"starHighlightCompression", s.starHighlightCompression},
        {"starTwinkleAmount", s.starTwinkleAmount},
        {"starGlow", s.starGlow},
        {"starTwinkleRate", s.starTwinkleRate},
        {"starCoreScale", s.starCoreScale},
        {"starSpikes", s.starSpikes},
        {"starSpikeLength", s.starSpikeLength},
        {"starShape", s.starShape},
        {"starObserver", s.starObserver == 0 ? "Eye" : "Camera"},
        {"iblIntensity", s.iblIntensity},
        {"iblLowerHemisphereDarkness", s.iblLowerHemisphereDarkness},
        {"skyMode", s.skyMode},
        {"groundAlbedo", Rgb(s.groundAlbedo)},
        {"groundNightColor", Rgb(s.groundNightColor)},
        {"groundBrightness", s.groundBrightness},
        {"belowHorizonMode", s.belowHorizonMode},
        {"belowHorizonBlendSharpness", s.belowHorizonBlendSharpness},
        {"belowHorizonDarkness", s.belowHorizonDarkness},
        {"belowHorizonDarkColor", Rgb(s.belowHorizonDarkColor)},
        {"groundHazeStrength", s.groundHazeStrength},
        {"groundHorizonColor", Rgb(s.groundHorizonColor)},
        {"groundHorizonNightColor", Rgb(s.groundHorizonNightColor)},
        {"nightSkyHorizonColor", Rgb(s.nightSkyHorizonColor)},
        {"groundHorizonCosWidth", s.groundHorizonCosWidth},
        {"groundHorizonNightCosWidth", s.groundHorizonNightCosWidth}};
}

} // namespace

void RegisterSkyDebugHandlers(EditorDebugServer& server)
{
    // get_sky_settings — the SkySettings the sky feature holds for this frame. "active" is false
    // when no sky is drawn (no enabled Sky Environment, or an HDRI skybox without one).
    server.RegisterHandler("get_sky_settings", [](const EditorDebugServer::RequestContext&) -> json
    {
        auto* renderServices = EngineCore::GetInstance().GetRenderServices();
        const auto* sky = renderServices ? renderServices->GetFeature<Engine::Renderer::SkyRenderFeature>() : nullptr;
        if (!sky)
            return Editor::RefuseRequest("No sky feature: the renderer has not created one");
        if (!sky->HasActiveSettings())
            return json{{"active", false}};
        return json{{"active", true}, {"settings", DescribeSkySettings(sky->GetSettings())}};
    });
}

} // namespace GameEngine
