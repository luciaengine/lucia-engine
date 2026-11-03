#include "SceneView/ReflectionProbeGizmo.h"

#include <algorithm>
#include <cmath>

#include "Components/Rendering/ReflectionProbe.h"
#include "Components/Transform.h"
#include "Core/Engine.h"
#include "ECS/Components.h"
#include "ECS/ECS.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Query.h"
#include "Mathematics/Matrix4x4.h"
#include "Mathematics/Vector3.h"
#include "Types/Color.h"

namespace GameEngine::Editor::SceneTools
{

using GameEngine::Components::ReflectionProbe;
using GameEngine::Components::WorldTransform;
using GameEngine::Mathematics::Vector3;

namespace
{

constexpr float kMinProbeExtent = 0.0001f;
constexpr int kEllipseSegments = 48;

struct ProbeVolume
{
    Vector3 Center{0.0f, 0.0f, 0.0f};
    Vector3 AxisX{1.0f, 0.0f, 0.0f};
    Vector3 AxisY{0.0f, 1.0f, 0.0f};
    Vector3 AxisZ{0.0f, 0.0f, 1.0f};
    Vector3 HalfExtents{0.0f, 0.0f, 0.0f};
    bool Valid = false;
};

ProbeVolume ExtractProbeVolume(const WorldTransform& xf)
{
    ProbeVolume volume{};
    const float* m = xf.matrix;
    volume.Center = Vector3(m[12], m[13], m[14]);

    const Vector3 ax(m[0], m[1], m[2]);
    const Vector3 ay(m[4], m[5], m[6]);
    const Vector3 az(m[8], m[9], m[10]);
    const float sx = ax.Length();
    const float sy = ay.Length();
    const float sz = az.Length();
    if (sx <= kMinProbeExtent || sy <= kMinProbeExtent || sz <= kMinProbeExtent)
        return volume;

    volume.AxisX = ax / sx;
    volume.AxisY = ay / sy;
    volume.AxisZ = az / sz;
    volume.HalfExtents = Vector3(sx, sy, sz) * 0.5f;
    volume.Valid = true;
    return volume;
}

// Rotation-only probe frame: unit axes as columns, centre as translation.
Mathematics::Matrix4x4 MakeVolumeTransform(const ProbeVolume& volume)
{
    Mathematics::Matrix4x4 transform;
    transform[0] = glm::vec4(volume.AxisX.x, volume.AxisX.y, volume.AxisX.z, 0.0f);
    transform[1] = glm::vec4(volume.AxisY.x, volume.AxisY.y, volume.AxisY.z, 0.0f);
    transform[2] = glm::vec4(volume.AxisZ.x, volume.AxisZ.y, volume.AxisZ.z, 0.0f);
    transform[3] = glm::vec4(volume.Center.x, volume.Center.y, volume.Center.z, 1.0f);
    return transform;
}

void DrawProbeIcon(GizmoRenderContext& ctx,
                   const Vector3& pos,
                   const Vector3& right,
                   const Vector3& up,
                   float radius,
                   const Color& color,
                   float thickness)
{
    GizmoLineBatch batch(ctx, color, thickness);

    DrawCircleInto(batch, pos, right, radius, up, radius, 32);
    DrawCircleInto(batch, pos, right, radius * 0.58f, up, radius, 24);

    auto point = [&](float x, float y)
    {
        return pos + right * x * radius + up * y * radius;
    };

    batch.AddLine(point(-0.78f, 0.0f), point(0.78f, 0.0f));
    batch.AddLine(point(0.0f, -0.78f), point(0.0f, 0.78f));

    batch.AddLine(point(-0.35f, 0.35f), point(0.35f, -0.35f));
    batch.AddLine(point(-0.35f, -0.35f), point(0.35f, 0.35f));
}

} // namespace

void ReflectionProbeGizmo::SetSelection(const std::vector<GameEngine::ECS::EntityHandle>& entities)
{
    m_SelectedEntities = entities;
}

void ReflectionProbeGizmo::SetHovered(GameEngine::ECS::EntityHandle entity)
{
    m_HoveredEntity = entity;
}

void ReflectionProbeGizmo::Render(GizmoRenderContext& context)
{
    GameEngine::ECS::World* world = context.GetWorld();
    if (!world)
        return;

    const Vector3* camPosPtr = context.GetCameraWorldPosition();
    if (!camPosPtr)
        return;
    const Vector3& camPos = *camPosPtr;
    const float orthoHeight = (context.HasOrthoHeight() && context.GetOrthoHeight() > 0.0f)
                                  ? context.GetOrthoHeight()
                                  : 0.0f;

    world->Query<
             GameEngine::ECS::Read<ReflectionProbe>,
             GameEngine::ECS::Read<WorldTransform>>()
        .Each([&](GameEngine::ECS::EntityHandle e,
                  const ReflectionProbe& probe,
                  const WorldTransform& xf)
              {
                  const bool isHovered = m_HoveredEntity.IsValid() && e == m_HoveredEntity;
                  const bool isSelected = std::find(m_SelectedEntities.begin(),
                                                    m_SelectedEntities.end(),
                                                    e) != m_SelectedEntities.end();
                  const bool emphasize = isHovered || isSelected;

                  const ProbeVolume volume = ExtractProbeVolume(xf);
                  if (!volume.Valid)
                      return;

                  const Vector3& pos = volume.Center;
                  Vector3 right;
                  Vector3 up;
                  BuildBillboardBasis(pos, camPos, right, up);

                  const float iconRadius = ComputeGizmoIconWorldRadius(pos, camPos, orthoHeight);
                  const Color iconColor(emphasize ? 0.65f : 0.36f,
                                        emphasize ? 0.90f : 0.76f,
                                        1.0f,
                                        emphasize ? 1.0f : 0.78f);
                  DrawProbeIcon(context, pos, right, up, iconRadius, iconColor, emphasize ? 2.0f : 1.35f);

                  if (!emphasize)
                      return;

                  const Color influenceColor(0.28f, 0.78f, 1.0f, isSelected ? 0.82f : 0.55f);
                  const Color blendColor(0.28f, 0.78f, 1.0f, isSelected ? 0.32f : 0.22f);
                  const Vector3& halfExtents = volume.HalfExtents;

                  if (probe.InfluenceShape == Components::ReflectionProbeInfluenceShape::Box)
                  {
                      ProbeVolume projection = volume;
                      const float scale[3] = {probe.ProjectionScaleX, probe.ProjectionScaleY, probe.ProjectionScaleZ};
                      for (int i = 0; i < 3; ++i)
                      {
                          projection.Center[i] += volume.AxisX[i] * probe.ProjectionOffsetX +
                              volume.AxisY[i] * probe.ProjectionOffsetY + volume.AxisZ[i] * probe.ProjectionOffsetZ;
                          projection.HalfExtents[i] *= std::max(0.001f, scale[i]);
                      }
                      const auto transform = MakeVolumeTransform(projection);
                      const Vector3 lo = {-projection.HalfExtents[0], -projection.HalfExtents[1], -projection.HalfExtents[2]};
                      const Vector3 hi = {projection.HalfExtents[0], projection.HalfExtents[1], projection.HalfExtents[2]};
                      const Color projectionColor = {1.0f, 0.65f, 0.18f, 0.7f};
                      context.DrawTransformedWireBox(lo, hi, transform, projectionColor, 1.0f);
                  }
                  const Vector3 origin = {
                      pos[0] + volume.AxisX[0] * probe.OriginOffsetX + volume.AxisY[0] * probe.OriginOffsetY + volume.AxisZ[0] * probe.OriginOffsetZ,
                      pos[1] + volume.AxisX[1] * probe.OriginOffsetX + volume.AxisY[1] * probe.OriginOffsetY + volume.AxisZ[1] * probe.OriginOffsetZ,
                      pos[2] + volume.AxisX[2] * probe.OriginOffsetX + volume.AxisY[2] * probe.OriginOffsetY + volume.AxisZ[2] * probe.OriginOffsetZ};
                  const Color originColor = {1.0f, 0.82f, 0.28f, 0.86f};
                  context.DrawColoredLine(pos, origin, originColor, 1.2f);
                  context.DrawWireSphere(origin, iconRadius * 0.45f, originColor, 1.0f);
                  const bool boxInfluence = probe.InfluenceShape == Components::ReflectionProbeInfluenceShape::Box;
                  if (boxInfluence)
                  {
                      const Mathematics::Matrix4x4 volumeTransform = MakeVolumeTransform(volume);
                      const Vector3 boxMin = halfExtents * -1.0f;
                      context.DrawTransformedWireBox(boxMin, halfExtents, volumeTransform, influenceColor,
                                                     isSelected ? 1.6f : 1.2f);

                      const float blend = std::clamp(
                          probe.BlendDistance,
                          0.0f,
                          std::min({halfExtents.x, halfExtents.y, halfExtents.z}));
                      if (blend > 0.0f)
                      {
                          const Vector3 innerMax(std::max(0.0f, halfExtents.x - blend),
                                                 std::max(0.0f, halfExtents.y - blend),
                                                 std::max(0.0f, halfExtents.z - blend));
                          if (innerMax.x > 0.0f && innerMax.y > 0.0f && innerMax.z > 0.0f)
                          {
                              const Vector3 innerMin = innerMax * -1.0f;
                              context.DrawTransformedWireBox(innerMin, innerMax, volumeTransform, blendColor,
                                                             1.0f);
                          }
                      }

                      return;
                  }

                  DrawWireEllipsoid(context, volume.Center, volume.AxisX, volume.AxisY, volume.AxisZ, halfExtents,
                                    influenceColor, isSelected ? 1.6f : 1.2f, kEllipseSegments);

                  const float influenceRadius = std::max({halfExtents.x, halfExtents.y, halfExtents.z});
                  const float blend = std::clamp(probe.BlendDistance, 0.0f, influenceRadius);
                  if (blend > 0.0f)
                  {
                      const Vector3 innerHalf(std::max(0.0f, halfExtents.x - blend),
                                              std::max(0.0f, halfExtents.y - blend),
                                              std::max(0.0f, halfExtents.z - blend));
                      if (innerHalf.x > 0.0f && innerHalf.y > 0.0f && innerHalf.z > 0.0f)
                          DrawWireEllipsoid(context, volume.Center, volume.AxisX, volume.AxisY, volume.AxisZ,
                                            innerHalf, blendColor, 1.0f, kEllipseSegments);
                  }
              });
}

} // namespace GameEngine::Editor::SceneTools

namespace GameEngine::Editor::SceneTools
{
}
