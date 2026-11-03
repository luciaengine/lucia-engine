#include "Markups/MarkupPicking.h"

#include "Core/Application.h"
#include "Core/CpuProfiler.h"
#include "ECS/World.h"
#include "Markups/MarkupDrawList.h"
#include "Markups/MarkupEditorBridge.h"
#include "Markups/MarkupPresentation.h"
#include "Mathematics/Geometry.h"
#include "Mathematics/Vector2.h"
#include "Mathematics/Vector3.h"
#include "SceneView/SceneViewEvents.h"
#include "SceneView/SceneViewProjection.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>
#include <span>
#include <vector>

namespace GameEngine::Editor
{

using Components::MarkupVolumeShape;
using Mathematics::Vector2;
using Mathematics::Vector3;

namespace
{

// How far from a mark-up's outline, in view pixels, a click still picks it.
constexpr float kMarkupPickTolerancePixels = 6.0f;
// How many segments stand for a sphere's circle; a 32-gon strays from the true circle by under
// 0.5 % of its radius.
constexpr int kOutlineCircleSegments = 32;
// The view depth, in meters, a segment is clipped to before it projects: a perspective projection
// refuses a point nearer than 1 cm and an orthographic one would project a point behind the
// camera, so a segment that passes beside or behind the camera keeps only the part in front of it.
constexpr float kOutlineNearClipDepth = 0.05f;

// The outline point nearest the camera found under the pointer so far.
struct MarkupOutlineHit
{
    explicit MarkupOutlineHit(const SceneTools::ScenePointerEvent& event)
        : Event(event), Pointer(event.viewX, event.viewY)
    {
    }

    const SceneTools::ScenePointerEvent& Event;
    Vector2 Pointer;
    float Distance = std::numeric_limits<float>::max(); // meters from the camera to Point
    Vector3 Point{};
    ECS::EntityHandle Entity{};
};

// Clips segment ab to the part at or beyond kOutlineNearClipDepth in front of the camera; false
// when no part is.
bool ClipSegmentToView(const SceneTools::ScenePointerEvent& event, Vector3& a, Vector3& b)
{
    const float depthA = Vector3::Dot(a - event.cameraPos, event.cameraForward) - kOutlineNearClipDepth;
    const float depthB = Vector3::Dot(b - event.cameraPos, event.cameraForward) - kOutlineNearClipDepth;
    if (depthA < 0.0f && depthB < 0.0f)
        return false;
    if (depthA < 0.0f)
        a = a + (b - a) * (depthA / (depthA - depthB));
    else if (depthB < 0.0f)
        b = b + (a - b) * (depthB / (depthB - depthA));
    return true;
}

// Takes `entity` as the hit when its outline segment ab passes within kMarkupPickTolerancePixels
// of the pointer nearer the camera than the hit so far. The segment's point under the pointer is
// taken at the view's parameter along it, which a perspective view shifts a little from the
// world's: close enough to order outlines that overlap on screen.
void TestOutlineSegment(MarkupOutlineHit& hit, ECS::EntityHandle entity, Vector3 a, Vector3 b)
{
    if (!ClipSegmentToView(hit.Event, a, b))
        return;
    Vector2 pixelA;
    Vector2 pixelB;
    if (!SceneTools::ProjectWorldToView(hit.Event, a, pixelA) || !SceneTools::ProjectWorldToView(hit.Event, b, pixelB))
        return;
    const float t = Mathematics::ClosestSegmentParameter(pixelA, pixelB, hit.Pointer);
    const float offsetX = pixelA.x + (pixelB.x - pixelA.x) * t - hit.Pointer.x;
    const float offsetY = pixelA.y + (pixelB.y - pixelA.y) * t - hit.Pointer.y;
    if (offsetX * offsetX + offsetY * offsetY > kMarkupPickTolerancePixels * kMarkupPickTolerancePixels)
        return;
    const Vector3 point = a + (b - a) * t;
    const float distance = (point - hit.Event.cameraPos).Length();
    if (distance >= hit.Distance)
        return;
    hit.Distance = distance;
    hit.Point = point;
    hit.Entity = entity;
}

// A path's draped line, or a region's rings at their base and along their top edge (an outline
// that is not closed, at its base only), as MarkupGizmo draws them.
void TestSplineShapeOutline(MarkupOutlineHit& hit, const MarkupDrawItem& item)
{
    const MarkupECS::MarkupRegionGround& ground = item.Region->Ground;
    if (item.Path)
    {
        for (std::size_t i = 0; i + 1 < ground.Outline.size(); ++i)
            TestOutlineSegment(hit, item.Entity, ground.Outline[i], ground.Outline[i + 1]);
        return;
    }
    const Vector3 lift(0.0f, item.Region->ExtrudeHeight, 0.0f);
    // Cozy's display cache stores one region outline; use that same geometry
    // for picking rather than importing the newer multi-ring display model.
    {
        const std::span<const Vector3> ring = ground.Outline;
        if (ring.size() < 2)
            return;
        const std::size_t edges = ground.Closed ? ring.size() : ring.size() - 1;
        for (std::size_t i = 0; i < edges; ++i)
        {
            const Vector3& from = ring[i];
            const Vector3& to = ring[(i + 1) % ring.size()];
            TestOutlineSegment(hit, item.Entity, from, to);
            if (ground.Closed)
                TestOutlineSegment(hit, item.Entity, from + lift, to + lift);
        }
    }
}

// A box's twelve edges.
void TestBoxOutline(MarkupOutlineHit& hit, const MarkupDrawItem& item)
{
    const MarkupWorldVolume& volume = item.Volume;
    std::array<Vector3, 8> corners;
    for (std::size_t corner = 0; corner < corners.size(); ++corner)
    {
        Vector3 point = volume.Center;
        for (std::size_t axis = 0; axis < 3; ++axis)
        {
            const float sign = ((corner >> axis) & 1u) != 0 ? 1.0f : -1.0f;
            point = point + volume.Axes[axis] * (volume.HalfExtents[axis] * sign);
        }
        corners[corner] = point;
    }
    for (std::size_t corner = 0; corner < corners.size(); ++corner)
    {
        for (std::size_t axisBit = 1; axisBit < corners.size(); axisBit <<= 1)
        {
            if ((corner & axisBit) == 0)
                TestOutlineSegment(hit, item.Entity, corners[corner], corners[corner | axisBit]);
        }
    }
}

// The circle of `radius` around `center` in the plane of the unit axes `u` and `v`.
void TestOutlineCircle(MarkupOutlineHit& hit, ECS::EntityHandle entity, const Vector3& center, const Vector3& u,
                       const Vector3& v, float radius)
{
    Vector3 previous = center + u * radius;
    for (int i = 1; i <= kOutlineCircleSegments; ++i)
    {
        const float angle = 2.0f * std::numbers::pi_v<float> * static_cast<float>(i) /
                            static_cast<float>(kOutlineCircleSegments);
        const Vector3 next = center + (u * std::cos(angle) + v * std::sin(angle)) * radius;
        TestOutlineSegment(hit, entity, previous, next);
        previous = next;
    }
}

// A sphere's circle facing the camera, its silhouette; from inside it, its three rings.
void TestSphereOutline(MarkupOutlineHit& hit, const MarkupDrawItem& item)
{
    const MarkupWorldVolume& volume = item.Volume;
    const float radius = volume.BoundingRadius;
    if (item.CameraInside)
    {
        TestOutlineCircle(hit, item.Entity, volume.Center, volume.Axes[0], volume.Axes[1], radius);
        TestOutlineCircle(hit, item.Entity, volume.Center, volume.Axes[1], volume.Axes[2], radius);
        TestOutlineCircle(hit, item.Entity, volume.Center, volume.Axes[0], volume.Axes[2], radius);
        return;
    }
    Vector3 toCamera = (hit.Event.cameraPos - volume.Center).NormalizeOrZero();
    if (toCamera.LengthSquared() == 0.0f)
        toCamera = Vector3(0.0f, 1.0f, 0.0f);
    Vector3 u = Vector3::Cross(toCamera, hit.Event.cameraUp).NormalizeOrZero();
    if (u.LengthSquared() == 0.0f)
        u = hit.Event.cameraRight;
    const Vector3 v = Vector3::Cross(u, toCamera).NormalizeOrZero();
    TestOutlineCircle(hit, item.Entity, volume.Center, u, v, radius);
}

} // namespace

SceneTools::SceneOverlayHit PickMarkupOutline(ECS::World& world, const MarkupEditorBridge& bridge,
                                              const SceneTools::ScenePointerEvent& event, uint64 frame,
                                              const SceneTools::SceneOverlayPickFilter& isPickable)
{
    GE_CPU_PROFILE_SCOPE("Markups.PickOutline");
    std::vector<MarkupDrawItem> items;
    CollectMarkupDrawItems(world, bridge, bridge.GetHighlight(), event.cameraPos, nullptr, frame, items);
    MarkupOutlineHit hit(event);
    for (const MarkupDrawItem& item : items)
    {
        if (isPickable && !isPickable(item.Entity))
            continue;
        if (item.Region)
            TestSplineShapeOutline(hit, item);
        else if (item.Volume.Shape == MarkupVolumeShape::Sphere)
            TestSphereOutline(hit, item);
        else
            TestBoxOutline(hit, item);
    }
    return {hit.Entity, hit.Point};
}

SceneTools::SceneOverlayHit PickSceneMarkupOutline(ECS::World& world, const SceneTools::ScenePointerEvent& event,
                                                   const SceneTools::SceneOverlayPickFilter& isPickable)
{
    const MarkupEditorBridge* bridge = MarkupEditorBridge::TryGet();
    if (!bridge)
        return {};
    const Application* application = Application::Get();
    return PickMarkupOutline(world, *bridge, event, application ? application->GetFrameCount() : 0, isPickable);
}

} // namespace GameEngine::Editor
