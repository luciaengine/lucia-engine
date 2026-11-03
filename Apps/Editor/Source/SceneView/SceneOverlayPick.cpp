#include "SceneView/SceneOverlayPick.h"

#include "Mathematics/Ray.h"
#include "Picking/MeshPickingService.h"
#include "SceneView/SceneViewEvents.h"

#include <algorithm>
#include <utility>

namespace GameEngine::Editor::SceneTools
{

namespace
{
// How much nearer than an outline point, along the ray to it, scene geometry must lie to hide it:
// 0.1 % of the point's distance, at least 5 cm. An outline drawn on the ground it was draped on
// (a region's base ring) meets that ground at its own point, so float error and the coarser
// surface the raycast meets must not count as cover.
constexpr float kOcclusionMinTolerance = 0.05f;
constexpr float kOcclusionTolerancePerMeter = 0.001f;

SceneOverlayPicker& OverlayPicker()
{
    static SceneOverlayPicker picker;
    return picker;
}

// Whether scene geometry lies between the camera of `event` and `point`: a ray from the camera's
// eye (perspective) or from the camera plane along its forward (orthographic) to the point.
bool IsHiddenByScene(ECS::World& world, const ScenePointerEvent& event, const Mathematics::Vector3& point)
{
    Mathematics::Ray3D ray;
    float pointDistance = 0.0f;
    if (event.tanHalfFovY > 0.0f)
    {
        const Mathematics::Vector3 toPoint = point - event.cameraPos;
        pointDistance = toPoint.Length();
        if (pointDistance <= 0.0f)
            return false;
        ray.origin = event.cameraPos;
        ray.direction = toPoint * (1.0f / pointDistance);
    }
    else
    {
        pointDistance = Mathematics::Vector3::Dot(point - event.cameraPos, event.cameraForward);
        ray.origin = point - event.cameraForward * pointDistance;
        ray.direction = event.cameraForward;
    }
    Picking::PickOptions options;
    options.WaitForSceneTlas = false; // the caller saw the scene TLAS ready
    options.MaxDistance = pointDistance;
    const Picking::PickResult cover = Picking::RaycastScene(ray, world, options);
    const float tolerance = std::max(kOcclusionMinTolerance, pointDistance * kOcclusionTolerancePerMeter);
    return cover.Hit && cover.Best.Distance < pointDistance - tolerance;
}
} // namespace

void SetSceneOverlayPicker(SceneOverlayPicker picker)
{
    OverlayPicker() = std::move(picker);
}

ECS::EntityHandle PickSceneOverlay(ECS::World& world, const ScenePointerEvent& event,
                                   const SceneOverlayPickFilter& isPickable)
{
    const SceneOverlayPicker& picker = OverlayPicker();
    if (!picker)
        return {};
    const SceneOverlayHit hit = picker(world, event, isPickable);
    if (!hit.Entity.IsValid() || IsHiddenByScene(world, event, hit.Point))
        return {};
    return hit.Entity;
}

} // namespace GameEngine::Editor::SceneTools
