#include "Engine/Rendering/Impostors/ImpostorFrameCamera.h"

#include "Engine/Rendering/RenderOrigin.h"
#include "Mathematics/MatrixOps.h"
#include "Mathematics/WorldPosition.h"

#include <algorithm>
#include <cmath>

namespace GameEngine::Engine::Renderer
{
namespace
{

// Views closer to the pole than this (cosine) take +Z as up.
constexpr float kPoleCosine = 0.99f;
// The smallest bounding radius a frame camera fits, meters.
constexpr float kMinimumRadius = 0.01f;

} // namespace

ImpostorFrameAxes MakeImpostorFrameAxes(const Mathematics::Vector3& viewDirection)
{
    const Mathematics::Vector3 worldUp = std::abs(viewDirection.y) > kPoleCosine ? Mathematics::Vector3(0.0f, 0.0f, 1.0f)
                                                                                 : Mathematics::Vector3(0.0f, 1.0f, 0.0f);
    // Left-handed: the camera looks along -viewDirection, right = up x forward.
    const Mathematics::Vector3 forward = viewDirection * -1.0f;
    const Mathematics::Vector3 right = Mathematics::Vector3::Cross(worldUp, forward).Normalize();
    return ImpostorFrameAxes{right, Mathematics::Vector3::Cross(forward, right)};
}

Rendering::CameraData MakeImpostorFrameCamera(Mathematics::OctahedralLayout layout,
                                              uint32_t column,
                                              uint32_t row,
                                              uint32_t framesPerSide,
                                              const Mathematics::Vector3& center,
                                              float sphereRadius)
{
    const Mathematics::Vector3 direction = Mathematics::OctahedralFrameDirection(layout, column, row, framesPerSide);
    const float radius = std::max(sphereRadius, kMinimumRadius);
    const Mathematics::Vector3 eye = center + direction * (2.0f * radius);
    const ImpostorFrameAxes axes = MakeImpostorFrameAxes(direction);

    Rendering::CameraData camera{};
    camera.view = Mathematics::MakeLookAtLH(eye, center, axes.Up);
    camera.proj = Mathematics::MakeOrthographicLH_ZO_ReverseZ(-radius, radius, -radius, radius, radius, 3.0f * radius);
    camera.viewProj = camera.proj * camera.view;
    SetCameraEye(camera, Mathematics::WorldPosition::FromVector3(eye));
    return camera;
}

} // namespace GameEngine::Engine::Renderer
