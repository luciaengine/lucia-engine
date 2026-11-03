#include "Editor/Entities/EntityComponentTraits.h"

#include "Components/Hierarchy.h"
#include "Components/Name.h"
#include "Components/RuntimeOnlyEntity.h"
#include "Components/SceneBlueprintInstance.h"
#include "Components/SceneEntityTag.h"
#include "Components/SceneSubsceneInstance.h"
#include "Components/Transform.h"
#include "ECS/ECS.h"
#include "Editor/Entities/EditorComponentTraits.h"

#include <utility>

namespace GameEngine::Editor
{

void RegisterEntityComponentTraits()
{
    constexpr int kNameSortKey = 10;
    constexpr int kParentSortKey = 20;

    // Name is shown and edited in the inspector header.
    {
        EditorComponentTraits traits;
        traits.DisplayName = "Name";
        traits.SortKey = kNameSortKey;
        traits.HiddenInInspector = true;
        traits.Removable = false;
        EditorComponentTraitsRegistry::Get().Register(
            ECS::GetComponentTypeId<Components::Name>(), std::move(traits));
    }

    // The Hierarchy panel manages the parent link.
    {
        EditorComponentTraits traits;
        traits.DisplayName = "Parent";
        traits.SortKey = kParentSortKey;
        traits.HiddenInInspector = true;
        EditorComponentTraitsRegistry::Get().Register(
            ECS::GetComponentTypeId<Components::Parent>(), std::move(traits));
    }

    // Computed from Transform and the hierarchy every frame.
    {
        EditorComponentTraits traits;
        traits.DisplayName = "World Transform";
        traits.HiddenInInspector = true;
        EditorComponentTraitsRegistry::Get().Register(
            ECS::GetComponentTypeId<Components::WorldTransform>(), std::move(traits));
    }

    // Sibling order, maintained by hierarchy reparent and reorder.
    {
        EditorComponentTraits traits;
        traits.HiddenInInspector = true;
        EditorComponentTraitsRegistry::Get().Register(
            ECS::GetComponentTypeId<Components::HierarchyOrder>(), std::move(traits));
    }

    // The scene serialization id SceneIO assigns.
    {
        EditorComponentTraits traits;
        traits.DisplayName = "Scene Entity Tag";
        traits.HiddenInInspector = true;
        EditorComponentTraitsRegistry::Get().Register(
            ECS::GetComponentTypeId<Components::SceneEntityTag>(), std::move(traits));
    }

    // Generator output; the notice above the sections says so.
    {
        EditorComponentTraits traits;
        traits.HiddenInInspector = true;
        EditorComponentTraitsRegistry::Get().Register(
            ECS::GetComponentTypeId<Components::RuntimeOnlyEntity>(), std::move(traits));
    }

    {
        EditorComponentTraits traits;
        traits.DisplayName = "Scene Blueprint Instance";
        EditorComponentTraitsRegistry::Get().Register(
            ECS::GetComponentTypeId<Components::SceneBlueprintInstance>(), std::move(traits));
    }

    {
        EditorComponentTraits traits;
        traits.DisplayName = "Scene Subscene Instance";
        EditorComponentTraitsRegistry::Get().Register(
            ECS::GetComponentTypeId<Components::SceneSubsceneInstance>(), std::move(traits));
    }
}

} // namespace GameEngine::Editor
