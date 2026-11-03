#include "Engine/Rendering/RenderSorting.h"

#include "Components/Hierarchy.h"
#include "Components/Rendering/RenderSorting.h"
#include "Components/Transform.h"
#include "Engine/Rendering/RenderOrigin.h"
#include "Engine/Rendering/SortingLayers.h"
#include "ECS/World.h"
#include "ECS/ECSTemplates.h"

namespace GameEngine::Engine::Renderer
{
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
                result.Authored = sorting->Layer != 0 || sorting->OrderInLayer != 0 || sorting->GroupChildren;
            }
            if (sorting->GroupChildren)
            {
                result.Authored = true;
                result.Grouped = true;
                result.Layer = layers.Priority(sorting->Layer);
                result.Order = sorting->OrderInLayer;
                result.Group = current;
                for (auto& coordinate : result.GroupPosition)
                    coordinate = 0;
                if (const auto* transform = world.GetComponent<Components::WorldTransform>(current))
                {
                    for (uint32 axis = 0; axis < 3; ++axis)
                        result.GroupPosition[axis] = transform->matrix[12 + axis];
                    if (const auto* sector = world.GetComponent<Components::WorldSectorCoord>(current))
                    {
                        float32 x, y, z;
                        SectorToWorld(*sector, x, y, z);
                        result.GroupPosition[0] += x;
                        result.GroupPosition[1] += y;
                        result.GroupPosition[2] += z;
                    }
                }
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
