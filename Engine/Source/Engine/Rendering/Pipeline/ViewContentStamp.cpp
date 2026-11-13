#include "Engine/Rendering/Pipeline/ViewContentStamp.h"

#include "Engine/Rendering/RenderServices.h"

#include <cstring>

namespace GameEngine::Engine::Renderer::Pipeline
{
ViewContentStamp ViewContentStamp::Capture(const RenderServices& rs, uint64_t worldId)
{
    ViewContentStamp stamp;
    stamp.WorldId = worldId;
    stamp.RenderContentVersion = rs.RenderContentVersion(worldId);
    stamp.ShadowCasterContentVersion = rs.ShadowCasterContentVersion(worldId);
    stamp.LightListVersion = rs.WorldLightListVersion(worldId);
    stamp.AppFrame = rs.Spine().WorldFrameEpoch();
    return stamp;
}

bool ViewContentStamp::SameContent(const ViewContentStamp& other) const
{
    return WorldId == other.WorldId && RenderContentVersion == other.RenderContentVersion &&
           ShadowCasterContentVersion == other.ShadowCasterContentVersion &&
           LightListVersion == other.LightListVersion;
}

bool ViewContentStamp::ViewIsStill(const ViewContentStamp& previous,
                                   const ViewContentStamp& current,
                                   const Rendering::CameraData& previousCamera,
                                   const Rendering::CameraData& currentCamera, bool viewHasMovers)
{
    return current.WorldId != 0u && previous.SameContent(current) &&
           current.AppFrame == previous.AppFrame + 1u && !viewHasMovers &&
           std::memcmp(previousCamera.viewProj.Data(), currentCamera.viewProj.Data(),
                       sizeof(currentCamera.viewProj)) == 0;
}
} // namespace GameEngine::Engine::Renderer::Pipeline
