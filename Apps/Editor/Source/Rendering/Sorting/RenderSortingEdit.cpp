#include "Rendering/Sorting/RenderSortingEditor.h"

#include "Components/Rendering/RenderSorting.h"
#include "ECS/World.h"
#include "ECS/ECSTemplates.h"
#include "Inspectors/InspectorComponentRowHelpers.h"
#include "UndoRedo/UndoRedoService.h"

#include <algorithm>
#include <cstring>

namespace GameEngine::Editor
{
namespace
{
using Sorting = Components::RenderSorting;
std::vector<ECS::ComponentTypeId> g_RendererComponents;
const std::string kEditName = "Change Sorting Layer";

bool CaptureSorting(ECS::World& world, ECS::EntityHandle entity,
                    UndoRedoService::SnapshotTarget::Snapshot& bytes)
{
    if (!world.IsValid(entity))
        return false;
    if (const auto* value = world.GetComponent<Sorting>(entity))
    {
        bytes.resize(sizeof(Sorting));
        std::memcpy(bytes.data(), value, sizeof(Sorting));
    }
    else
        bytes.clear();
    return true;
}

bool RestoreSorting(ECS::World& world, ECS::EntityHandle entity,
                    const UndoRedoService::SnapshotTarget::Snapshot& bytes)
{
    if (!world.IsValid(entity))
        return false;
    if (bytes.empty())
    {
        world.RemoveComponentImmediate<Sorting>(entity);
        return true;
    }
    if (bytes.size() != sizeof(Sorting))
        return false;
    Sorting value{};
    std::memcpy(&value, bytes.data(), sizeof(Sorting));
    world.AddComponentImmediate(entity, value);
    return true;
}

}

bool SupportsRenderSorting(ECS::World& world, ECS::EntityHandle entity)
{
    if (!world.IsValid(entity))
        return false;
    if (world.HasComponent<Sorting>(entity))
        return true;
    return std::any_of(g_RendererComponents.begin(), g_RendererComponents.end(),
        [&world, entity](ECS::ComponentTypeId type) { return world.HasComponent(entity, type); });
}

void ApplyRenderSortingLayer(ECS::World& world, const std::vector<ECS::EntityHandle>& entities,
    int32 layer, UndoRedoService* undo, EditorChangeNotifications* notifications)
{
    if (undo)
        undo->BeginCompound(kEditName);
    for (const auto entity : entities)
    {
        if (!SupportsRenderSorting(world, entity))
            continue;
        const auto* current = world.GetComponent<Sorting>(entity);
        if ((current ? current->Layer : 0) == layer)
            continue;
        Sorting updated = current ? *current : Sorting{};
        updated.Layer = layer;
        if (undo)
        {
            auto target = InspectorDrag::MakeComponentSnapshotTarget<Sorting>(&world, entity, notifications, kEditName);
            target.Capture = [&world, entity](auto& bytes) { return CaptureSorting(world, entity, bytes); };
            target.Apply = [&world, entity](const auto& bytes) { return RestoreSorting(world, entity, bytes); };
            auto edit = undo->BeginInteractiveEdit(kEditName, std::move(target));
            world.AddComponentImmediate(entity, updated);
            edit.Commit();
        }
        else
            CommitComponentUpdate(&world, entity, notifications, updated);
    }
    if (undo)
        undo->EndCompound();
}

void RegisterRenderSortingComponent(ECS::ComponentTypeId component)
{
    if (std::find(g_RendererComponents.begin(), g_RendererComponents.end(), component) == g_RendererComponents.end())
        g_RendererComponents.push_back(component);
}

}
