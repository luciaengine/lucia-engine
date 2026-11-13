#include "DebugServer/Terrain/TerrainStatsReport.h"

#include "TerrainECS/TerrainPages.h"

#include <gtest/gtest.h>

using GameEngine::Editor::DescribePlanetPageTable;

// The reply names, per face, the entries of each level and the level each pinned page's center resolves to.
TEST(TerrainPlanetPageTableReport, ReportsEachFacesEntriesPerLevelAndThePinnedPagesResolvedLevels)
{
    GameEngine::TerrainECS::PageStreamCounts stream;
    stream.Planet = true;
    stream.Published = true;
    stream.PinnedLevel = 3;
    stream.PinnedPagesX = 2;
    stream.PinnedPagesZ = 1;
    stream.Faces.resize(6);
    stream.Faces[4].EntriesPerLevel = {5, 0, 0, 2, 1};
    stream.Faces[4].ResolvedLevel = {0, 3};

    const nlohmann::json r = DescribePlanetPageTable(stream);
    EXPECT_TRUE(r.at("published").get<bool>());
    EXPECT_EQ(r.at("pinnedLevel").get<uint32_t>(), 3u);
    EXPECT_EQ(r.at("pinnedPagesX").get<uint32_t>(), 2u);
    EXPECT_EQ(r.at("pinnedPagesZ").get<uint32_t>(), 1u);
    ASSERT_EQ(r.at("faces").size(), 6u);
    const nlohmann::json& face = r.at("faces")[4];
    ASSERT_TRUE(face.at("entriesPerLevel").is_array());
    EXPECT_EQ(face.at("entriesPerLevel")[3].get<uint32_t>(), 2u);
    ASSERT_TRUE(face.at("resolvedLevel").is_array()) << "a level list, not a byte string";
    ASSERT_EQ(face.at("resolvedLevel").size(), 2u);
    EXPECT_EQ(face.at("resolvedLevel")[0].get<uint32_t>(), 0u);
    EXPECT_EQ(face.at("resolvedLevel")[1].get<uint32_t>(), 3u);
}
