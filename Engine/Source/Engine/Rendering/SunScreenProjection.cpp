#include "Engine/Rendering/SunScreenProjection.h"

namespace GameEngine::Engine::Renderer
{

using namespace ::GameEngine::Rendering;

std::optional<Mathematics::Vector3> ProjectToNdc(const CameraData& camera, const Mathematics::Vector3& position)
{
    const float x = position.x;
    const float y = position.y;
    const float z = position.z;
    const float cx = camera.viewProj[0][0] * x + camera.viewProj[1][0] * y + camera.viewProj[2][0] * z + camera.viewProj[3][0];
    const float cy = camera.viewProj[0][1] * x + camera.viewProj[1][1] * y + camera.viewProj[2][1] * z + camera.viewProj[3][1];
    const float cz = camera.viewProj[0][2] * x + camera.viewProj[1][2] * y + camera.viewProj[2][2] * z + camera.viewProj[3][2];
    const float cw = camera.viewProj[0][3] * x + camera.viewProj[1][3] * y + camera.viewProj[2][3] * z + camera.viewProj[3][3];
    if (cw <= 1e-5f)
        return std::nullopt;
    return Mathematics::Vector3(cx / cw, cy / cw, cz / cw);
}

std::optional<Mathematics::Vector3> ProjectSunDirectionToNdc(const CameraData& camera, const Mathematics::Vector3& direction)
{
    if (camera.cameraPos.w < 0.5f)
    {
        const float cx = camera.viewProj[0][0] * direction.x + camera.viewProj[1][0] * direction.y +
                         camera.viewProj[2][0] * direction.z;
        const float cy = camera.viewProj[0][1] * direction.x + camera.viewProj[1][1] * direction.y +
                         camera.viewProj[2][1] * direction.z;
        const float cw = camera.viewProj[0][3] * direction.x + camera.viewProj[1][3] * direction.y +
                         camera.viewProj[2][3] * direction.z;
        if (cw <= 1e-5f)
            return std::nullopt;
        return Mathematics::Vector3(cx / cw, cy / cw, 0.0f);
    }

    constexpr float kOrthoSunDistance = 1000.0f;
    const Mathematics::Vector3 cameraPosition(camera.cameraPos.x, camera.cameraPos.y, camera.cameraPos.z);
    const std::optional<Mathematics::Vector3> ndc = ProjectToNdc(camera, cameraPosition + direction * kOrthoSunDistance);
    if (!ndc)
        return std::nullopt;
    return Mathematics::Vector3(ndc->x, ndc->y, 0.0f);
}

} // namespace GameEngine::Engine::Renderer
