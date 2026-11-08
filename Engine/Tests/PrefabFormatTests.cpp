#include "Scene/SceneDoc.h"

#include <string>

#include <gtest/gtest.h>

namespace GameEngine::Scene
{
namespace
{
std::string RepeatedAccent(std::size_t count)
{
    std::string text;
    for (std::size_t index = 0; index < count; ++index)
        text += "\xC3\xA9";
    return text;
}
} // namespace

TEST(PrefabFormat, EntityIdentityLimitCountsUtf8Bytes)
{
    const std::string header = "[prefab version=1]\n[entity id=\"root\"]\n";
    const std::string boundary = RepeatedAccent(kMaxPrefabIdLength / 2);
    SceneDoc accepted;
    std::string error;
    int line = 0;
    ASSERT_TRUE(ParseDocument("Boundary.prefab", header + "[entity id=\"" + boundary +
                                 "\" parent=\"root\"]\n", accepted, &error, &line)) << error;
    ASSERT_TRUE(ValidateDocument(accepted, &error, &line)) << error;

    SceneDoc refused;
    const std::string oversized = boundary + "\xC3\xA9";
    ASSERT_TRUE(ParseDocument("Oversized.prefab", header + "[entity id=\"" + oversized +
                                 "\" parent=\"root\"]\n", refused, &error, &line)) << error;
    EXPECT_FALSE(ValidateDocument(refused, &error, &line));
    EXPECT_EQ(line, 3);
    EXPECT_NE(error.find("48 bytes"), std::string::npos) << error;
    EXPECT_NE(error.find("Shorten the id to 46 bytes"), std::string::npos) << error;
}

TEST(PrefabFormat, OrdinarySceneEntityIdsKeepTheirExistingSyntaxAndLength)
{
    SceneDoc scene;
    std::string error;
    int line = 0;
    const std::string identity = "authored.segment/" + std::string(kMaxPrefabIdLength + 1, 'x');
    ASSERT_TRUE(ParseDocument("Existing.scene", "[scene version=1]\n[entity id=\"" + identity +
                                 "\"]\n", scene, &error, &line)) << error;
    EXPECT_TRUE(ValidateDocument(scene, &error, &line)) << error;
}

} // namespace GameEngine::Scene
