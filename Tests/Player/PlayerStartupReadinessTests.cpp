// The Player's startup load, and when its startup scene is ready for its startup line and its --screenshot
// capture: after the reveal, when the spline recipes are settled.

#include "PlayerStartupReadiness.h"

#include "Engine/Build/GameConfig.h"
#include "Engine/Loading/SceneLoadRequest.h"
#include "TestTempDir.h"

#include <gtest/gtest.h>

#include <fstream>

namespace GameEngine
{
namespace
{

constexpr float64 kFrameSeconds = 1.0 / 60.0;

TEST(PlayerStartupReadiness, TheSceneIsReadyWhenItIsRevealedAndTheRecipesAreSettled)
{
    PlayerStartupReadiness readiness;
    readiness.Begin(/*waitForSplineRecipes=*/true);

    EXPECT_FALSE(readiness.Update(/*revealed=*/false, /*recipesSettled=*/true, kFrameSeconds));
    EXPECT_FALSE(readiness.Update(true, false, kFrameSeconds));
    EXPECT_TRUE(readiness.IsPending());
    EXPECT_TRUE(readiness.Update(true, true, kFrameSeconds));

    EXPECT_FALSE(readiness.IsPending());
    EXPECT_EQ(readiness.Frames(), 3u);
    EXPECT_FALSE(readiness.Update(true, true, kFrameSeconds)) << "the scene becomes ready once";
}

// The wait has no clock of its own: a recipe Held past any time keeps the scene pending, and
// the recipe's own hold budget ends the wait.
TEST(PlayerStartupReadiness, TheRecipeWaitEndsOnlyWhenTheRecipesSettle)
{
    constexpr float64 kLongerThanAnyHoldBudget = 3600.0;
    PlayerStartupReadiness readiness;
    readiness.Begin(/*waitForSplineRecipes=*/true);

    EXPECT_FALSE(readiness.Update(true, false, kLongerThanAnyHoldBudget));
    EXPECT_TRUE(readiness.IsPending());
    EXPECT_TRUE(readiness.Update(true, true, kFrameSeconds));
    EXPECT_FALSE(readiness.IsPending());
}

// The world first draws when the startup scene is ready to show: the Player reveals the load at Ready,
// and SceneLoaderTests prove what PauseWorldDrawing does to the cameras.
TEST(PlayerStartupReadiness, TheStartupLoadPausesWorldDrawingUntilTheSceneIsReadyToShow)
{
    const SceneLoadRequest request = MakeStartupSceneLoadRequest("Scenes/Main.scene", [](SceneLoadInstantiated&) {});

    EXPECT_TRUE(request.PauseWorldDrawing);
    EXPECT_EQ(request.Reveal, SceneRevealPolicy::AtReady) << "nothing in the Player releases a held load";
    EXPECT_EQ(request.Mode, SceneLoadMode::Single);
    EXPECT_EQ(request.Scene, std::filesystem::path("Scenes/Main.scene"));
    EXPECT_TRUE(request.OnInstantiated);
}

TEST(PlayerStartupReadiness, WithoutTheRecipeWaitTheRevealAloneMakesTheSceneReady)
{
    PlayerStartupReadiness readiness;
    readiness.Begin(/*waitForSplineRecipes=*/false);

    EXPECT_TRUE(readiness.Update(/*revealed=*/true, /*recipesSettled=*/false, kFrameSeconds));
}

TEST(PlayerStartupReadiness, TheGameConfigWaitsForTheRecipesUnlessItSaysFalse)
{
    const TestUtils::ScopedTempDir root(TestUtils::MakeUniqueTempDirectory("PlayerStartupReadiness"));
    const std::filesystem::path configPath = root.Path() / "game.config";
    {
        std::ofstream file(configPath);
        file << R"({"startupScene": "Scenes/Main.scene", "waitForSplineRecipes": false})";
    }
    EXPECT_FALSE(LoadGameConfig(configPath).waitForSplineRecipes);

    GameConfig saved;
    ASSERT_TRUE(SaveGameConfig(configPath, saved));
    EXPECT_TRUE(LoadGameConfig(configPath).waitForSplineRecipes) << "the default waits";
    saved.waitForSplineRecipes = false;
    ASSERT_TRUE(SaveGameConfig(configPath, saved));
    EXPECT_FALSE(LoadGameConfig(configPath).waitForSplineRecipes);
}

TEST(PlayerStartupReadiness, ForestStandArchiveAndEntityRoundTrip)
{
    const TestUtils::ScopedTempDir root(TestUtils::MakeUniqueTempDirectory("PlayerStartupForest"));
    const auto configPath = root.Path() / "game.config";
    GameConfig config;
    ASSERT_TRUE(SaveGameConfig(configPath, config));
    EXPECT_TRUE(LoadGameConfig(configPath).ForestStands.empty());
    config.ForestStands.push_back({"Forests/Forest.geforest", "Canopy", "Trees/Tree.glb", 275.0f, 64u});
    ASSERT_TRUE(SaveGameConfig(configPath, config));
    const auto loaded = LoadGameConfig(configPath);
    ASSERT_EQ(loaded.ForestStands.size(), 1u);
    EXPECT_EQ(loaded.ForestStands.front().Archive, "Forests/Forest.geforest");
    EXPECT_EQ(loaded.ForestStands.front().Entity, "Canopy");
    EXPECT_EQ(loaded.ForestStands.front().ImpostorModel, "Trees/Tree.glb");
    EXPECT_FLOAT_EQ(loaded.ForestStands.front().DetailDistance, 275.0f);
    EXPECT_EQ(loaded.ForestStands.front().NearMeshBudget, 64u);
}

TEST(PlayerStartupReadiness, ForestStandsRoundTripWithIndependentArchiveSettings)
{
    const TestUtils::ScopedTempDir root(TestUtils::MakeUniqueTempDirectory("PlayerStartupForestStands"));
    const auto configPath = root.Path() / "game.config";
    {
        std::ofstream file(configPath);
        file << R"({
            "forestStand": {"archive": "Forests/Old.geforest", "entity": "Old"},
            "forestStands": [
                {"archive": "Forests/Pines.geforest", "entity": "Pines", "impostorModel": "Trees/Pine.glb",
                 "detailDistance": 175, "nearMeshBudget": 24},
                {"archive": "Forests/Oaks.geforest", "entity": "Oaks"}
            ]
        })";
    }
    auto config = LoadGameConfig(configPath);
    ASSERT_EQ(config.ForestStands.size(), 2u);
    ASSERT_TRUE(SaveGameConfig(configPath, config));
    const auto loaded = LoadGameConfig(configPath);
    ASSERT_EQ(loaded.ForestStands.size(), 2u);
    EXPECT_EQ(loaded.ForestStands[0].Archive, "Forests/Pines.geforest");
    EXPECT_EQ(loaded.ForestStands[0].Entity, "Pines");
    EXPECT_EQ(loaded.ForestStands[0].ImpostorModel, "Trees/Pine.glb");
    EXPECT_FLOAT_EQ(loaded.ForestStands[0].DetailDistance, 175.0f);
    EXPECT_EQ(loaded.ForestStands[0].NearMeshBudget, 24u);
    EXPECT_EQ(loaded.ForestStands[1].Archive, "Forests/Oaks.geforest");
    EXPECT_EQ(loaded.ForestStands[1].Entity, "Oaks");
    EXPECT_TRUE(loaded.ForestStands[1].ImpostorModel.empty());
    EXPECT_EQ(loaded.ForestStands[1].NearMeshBudget, 0u);
}

} // namespace
} // namespace GameEngine
