#include "Markups/ConvertMarkupToRegionCommand.h"

#include "Components/Hierarchy.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "EditorChangeNotifications.h"
#include "Markups/MarkupPresentation.h"
#include "Markups/MarkupRegionPort.h"
#include "Mathematics/Matrix4x4.h"
#include "SplineECS/SplineService.h"

#include <algorithm>
#include <iterator>
#include <optional>
#include <utility>

namespace GameEngine::Editor
{

namespace
{

// The matrix that places the entity's parent in the world; the identity for a root.
Components::WorldTransform ParentPlacement(const ECS::World& world, ECS::EntityHandle entity)
{
    const auto* parent = world.GetComponent<Components::Parent>(entity);
    Components::WorldTransform placed{};
    if (!parent || !world.IsValid(parent->parent))
        return placed;
    const Mathematics::Matrix4x4 matrix = MarkupPlacement(world, parent->parent);
    std::copy_n(matrix.Data(), 16, placed.matrix);
    return placed;
}

} // namespace

ConvertMarkupToRegionCommand::ConvertMarkupToRegionCommand(std::string label, ECS::World& world,
                                                           ECS::EntityHandle entity,
                                                           EditorChangeNotifications* notifications,
                                                           std::vector<Mathematics::Vector2> knots,
                                                           Spline::SplineType type, float32 extrudeHeight)
    : m_Label(std::move(label)),
      m_World(world),
      m_Entity(entity),
      m_Notifications(notifications),
      m_Knots(std::move(knots)),
      m_Type(type),
      m_ExtrudeHeight(extrudeHeight)
{
    if (const auto* transform = world.GetComponent<Components::Transform>(entity))
        m_TransformBefore = *transform;
    if (const auto* placed = world.GetComponent<Components::WorldTransform>(entity))
        m_WorldBefore = *placed;
    else
        std::copy(std::begin(m_TransformBefore.matrix), std::end(m_TransformBefore.matrix),
                  std::begin(m_WorldBefore.matrix));
    if (const auto* volume = world.GetComponent<Components::MarkupVolume>(entity))
        m_VolumeBefore = *volume;
}

ConvertMarkupToRegionCommand::~ConvertMarkupToRegionCommand()
{
    if (!m_Made || m_Applied)
        return;
    if (auto* splines = SplineECS::SplineService::TryGet())
        splines->DestroySpline(SplineECS::SplineHandle(m_Spline.SplineDataIndex, m_Spline.SplineDataGeneration));
}

void ConvertMarkupToRegionCommand::Do()
{
    if (!m_World.IsValid(m_Entity))
        return;
    m_World.RemoveComponentImmediate<Components::MarkupVolume>(m_Entity);
    if (!m_Made)
    {
        // The region stands where the volume's base stood.
        const std::optional<MarkupWorldVolume> volume =
            MarkupWorldVolumeFromMatrix(m_VolumeBefore.Shape, m_WorldBefore.matrix);
        const float32 baseY = volume ? 2.0f * volume->Center.y - volume->TopY : m_WorldBefore.matrix[13];
        const RegionPlacement placement =
            NewRegionPlacement(m_Knots, ParentPlacement(m_World, m_Entity).matrix, baseY);
        m_TransformAfter = placement.Local;
        m_WorldAfter = placement.World;
        m_World.AddComponentImmediate(m_Entity, m_TransformAfter);
        AddRegionParts(m_World, m_Entity, m_WorldAfter.matrix, m_Knots, m_Type, m_ExtrudeHeight);
        m_Spline = *m_World.GetComponent<Components::SplineComponent>(m_Entity);
        m_Region = *m_World.GetComponent<Components::MarkupRegion>(m_Entity);
        m_Made = true;
    }
    else
    {
        m_World.AddComponentImmediate(m_Entity, m_TransformAfter);
        m_World.AddComponentImmediate(m_Entity, m_WorldAfter);
        m_World.AddComponentImmediate(m_Entity, m_Spline);
        m_World.AddComponentImmediate(m_Entity, m_Region);
    }
    m_Applied = true;
    Announce();
}

void ConvertMarkupToRegionCommand::Undo()
{
    if (!m_World.IsValid(m_Entity))
        return;
    m_World.RemoveComponentImmediate<Components::SplineComponent>(m_Entity);
    m_World.RemoveComponentImmediate<Components::MarkupRegion>(m_Entity);
    m_World.AddComponentImmediate(m_Entity, m_VolumeBefore);
    m_World.AddComponentImmediate(m_Entity, m_TransformBefore);
    m_World.AddComponentImmediate(m_Entity, m_WorldBefore);
    m_Applied = false;
    Announce();
}

// The entity's parts changed: the structure (panels, inspectors) and its transform, which the
// mark-up bridge stamps.
void ConvertMarkupToRegionCommand::Announce()
{
    NotifyWorldStructure(m_Notifications, &m_World);
    if (m_Notifications)
        m_Notifications->NotifyComponentChange<Components::Transform>(&m_World, m_Entity,
                                                                      EditorChangeNotifications::ChangeKind::UndoRedo);
}

} // namespace GameEngine::Editor
