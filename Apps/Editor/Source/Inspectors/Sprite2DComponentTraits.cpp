#include "Inspectors/Sprite2DComponentTraits.h"

#include "Rendering/Sorting/RenderSortingEditor.h"

#include "Components/Rendering/Sprite2D.h"
#include "Editor/Entities/EditorComponentTraits.h"

#include <utility>

namespace GameEngine
{

void RegisterSprite2DComponentTraits()
{
    Editor::RegisterRenderSortingComponent(ECS::GetComponentTypeId<Components::Sprite2D>());
    auto& registry = Editor::EditorComponentTraitsRegistry::Get();

    Editor::EditorComponentTraits sprite;
    sprite.DisplayName = "Sprite 2D";
    sprite.InspectorCategory = "Rendering";
    sprite.InspectorTooltip = "Draws a rectangle of a texture in the entity's XY plane, using the entity’s sorting layer "
                              "and order";
    sprite.HierarchyRowClass = "hierarchy-entity-sprite";
    registry.Register(ECS::GetComponentTypeId<Components::Sprite2D>(), std::move(sprite));


}

} // namespace GameEngine
