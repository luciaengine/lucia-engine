#include "Terrain/TerrainDrawnNotice.h"

#include "Components/Terrain/Terrain.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "Editor/Entities/EntityDisplayName.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "TerrainECS/TerrainRenderPublication.h"
#include "UI/Controls/InspectorNotice.h"

namespace GameEngine::Editor
{

std::string DescribeUndrawnTerrain(ECS::World& world, ECS::EntityHandle entity)
{
    const auto* terrain = world.GetComponent<Components::Terrain>(entity);
    if (!terrain || !TerrainECS::HasTerrainHandle(*terrain))
        return {};
    const ECS::Entity selected(&world, entity);
    if (!selected.IsEnabledInHierarchy() || !selected.IsEnabled<Components::Terrain>())
        return {};
    const ECS::EntityHandle drawn = TerrainECS::FindDrawnTerrain(world);
    if (!drawn.IsValid() || drawn == entity)
        return {};
    const std::string drawnName = "\"" + EntityDisplayName(world, drawn) + "\"";
    return "Only one terrain draws in a scene, and " + drawnName + " draws. To draw this terrain, disable or delete " +
           drawnName + ".";
}

void AddTerrainDrawnNotice(UIElement* parent, ECS::World& world, ECS::EntityHandle entity)
{
    if (!parent)
        return;
    const std::string text = DescribeUndrawnTerrain(world, entity);
    if (text.empty())
        return;
    InspectorUI::AddNotice(parent, text, EditorUI::InspectorNotice::Kind::Warning);
}

} // namespace GameEngine::Editor
