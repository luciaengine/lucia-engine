#include "Engine/Rendering/RenderSorting.h"

#include "Components/Hierarchy.h"
#include "Components/Rendering/RenderSorting.h"
#include "Components/Transform.h"
#include "Engine/Rendering/RenderOrigin.h"
#include "Engine/Rendering/SortingLayers.h"
#include "ECS/World.h"
#include "ECS/ECSTemplates.h"
#include "Mathematics/Matrix4x4.h"

namespace GameEngine::Engine::Renderer
{
namespace
{
Mathematics::Vector3 ResolveGroundPoint(ECS::World& world, ECS::EntityHandle entity, float32 originY)
{
    Mathematics::Vector3 point;
    if (const auto* transform = world.GetComponent<Components::WorldTransform>(entity))
    {
        point = Mathematics::Matrix4x4::FromColumnMajor(transform->matrix).TransformPoint({0, originY, 0});
        if (const auto* sector = world.GetComponent<Components::WorldSectorCoord>(entity))
        {
            Mathematics::Vector3 offset;
            SectorToWorld(*sector, offset.x, offset.y, offset.z);
            point = point + offset;
        }
    }
    return point;
}
}

RenderSortingData ResolveRenderSorting(ECS::World& world, ECS::EntityHandle entity,
                                       const Rendering::SortingLayers& layers, bool hasGroups)
{
    RenderSortingData result;
    result.Layer = layers.Priority(0);
    const auto type = ECS::GetComponentTypeId<Components::RenderSorting>();
    auto current = entity;
    // Bounded parent walking also handles malformed cyclic authoring.
    for (uint32 depth = 0; depth < 256 && world.IsValid(current); ++depth)
    {
        const auto* sorting = world.GetComponent<Components::RenderSorting>(current);
        if (sorting && world.IsComponentEnabled(current, type))
        {
            if (current == entity)
            {
                result.Layer = layers.Priority(sorting->Layer);
                result.Order = sorting->OrderInLayer;
                result.MemberOrder = sorting->OrderInLayer;
                result.SortByY = sorting->SortByY;
                result.SortOriginY = sorting->SortOriginY;
                if (result.SortByY)
                    result.SortY = ResolveGroundPoint(world, entity, sorting->SortOriginY).y;
                result.Authored = sorting->Layer != 0 || sorting->OrderInLayer != 0 ||
                                  sorting->GroupChildren || sorting->SortByY;
            }
            if (sorting->GroupChildren)
            {
                result.Authored = true;
                result.Grouped = true;
                result.Layer = layers.Priority(sorting->Layer);
                result.Order = sorting->OrderInLayer;
                result.Group = current;
                result.GroupSortByY = sorting->SortByY;
                result.GroupPosition = ResolveGroundPoint(world, current,
                    sorting->SortByY ? sorting->SortOriginY : 0.0f);
            }
        }
        if (!hasGroups)
            break;
        const auto* parent = world.GetComponent<Components::Parent>(current);
        if (!parent || parent->parent == current)
            break;
        current = parent->parent;
    }
    return result;
}
}
