// The Inspector notice on a terrain that is active but not drawn: only one terrain draws in a
// scene (#3013), so the other terrains say so and name the drawn one.

#include <gtest/gtest.h>

#include "Components/Name.h"
#include "Components/Terrain/Terrain.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Terrain/TerrainDrawnNotice.h"

#include <string>

using namespace GameEngine;

TEST(TerrainDrawnNotice, AnUndrawnTerrainNamesTheDrawnTerrainAndTheDrawnTerrainShowsNoNotice)
{
    ECS::World world;
    Components::Terrain live{};
    live.TerrainDataGeneration = 1;
    const ECS::EntityHandle drawn =
        world.Create<Components::Terrain, Components::Name>(live, {"Outer Terrain"}).GetHandle();
    const ECS::EntityHandle other =
        world.Create<Components::Terrain, Components::Name>(live, {"Near Field"}).GetHandle();

    EXPECT_EQ(Editor::DescribeUndrawnTerrain(world, other),
              "Only one terrain draws in a scene, and \"Outer Terrain\" draws. To draw this terrain, disable or "
              "delete \"Outer Terrain\".");
    EXPECT_EQ(Editor::DescribeUndrawnTerrain(world, drawn), "");
}
