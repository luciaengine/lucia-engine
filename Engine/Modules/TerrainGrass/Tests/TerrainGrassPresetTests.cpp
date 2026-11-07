#include "TerrainGrass/TerrainGrassPresets.h"

#include <gtest/gtest.h>

using namespace GameEngine;

TEST(TerrainGrassPresets, ApplyingStylizedPreservesRangeAndChangesTheLook)
{
    Components::TerrainGrass grass;
    grass.Range = 230.0f;

    TerrainGrass::ApplyTerrainGrassPreset(grass, TerrainGrass::GrassPreset::Stylized);

    EXPECT_FLOAT_EQ(grass.Range, 230.0f);
    EXPECT_FLOAT_EQ(grass.BladesPerSquareMeter, 128.0f);
    EXPECT_EQ(TerrainGrass::MatchTerrainGrassPreset(grass), TerrainGrass::GrassPreset::Stylized);

    grass.BladeHeight += 0.1f;
    EXPECT_EQ(TerrainGrass::MatchTerrainGrassPreset(grass), TerrainGrass::GrassPreset::Custom);
}
