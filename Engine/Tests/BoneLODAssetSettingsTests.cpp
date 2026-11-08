#include "Assets/BoneLOD/BoneLODAssetSettings.h"
#include "Assets/AssetRegistry.h"
#include "TestTempDir.h"

#include <fstream>
#include <limits>

#include <gtest/gtest.h>

using namespace GameEngine;

namespace
{

class BoneLODAssetSettingsTest : public testing::Test
{
protected:
    void SetUp() override
    {
        std::filesystem::create_directories(Assets);
        std::ofstream(Model) << "metadata fixture";
        ASSERT_TRUE(Registry.Initialize(Assets, nullptr, Temporary.Path() / "AssetDatabase.assetdb",
            Temporary.Path() / ".Cache"));
        Registry.WaitForStartupScan();
    }

    void TearDown() override { Registry.Shutdown(); }

    TestUtils::ScopedTempDir Temporary{TestUtils::MakeUniqueTempDirectory("bone_lod_settings")};
    const std::filesystem::path Assets = Temporary.Path() / "Assets";
    const std::filesystem::path Model = Assets / "Character.fbx";
    AssetRegistry Registry;
};

} // namespace

TEST_F(BoneLODAssetSettingsTest, MissingMetadataUsesDefaultBudgetWithoutMandatoryBones)
{
    const auto settings = BoneLODAssetSettings::Load(Registry, Model);
    EXPECT_EQ(settings.BoneBudget, 32u);
    EXPECT_TRUE(settings.MandatoryPaths.empty());
    EXPECT_TRUE(settings.Error.empty());
}

TEST_F(BoneLODAssetSettingsTest, StructuredPathsRoundTripSeparatorsQuotesAndUnnamedNodes)
{
    BoneLODAssetSettings authored;
    authored.BoneBudget = 47u;
    authored.MandatoryPaths = {{"Armature", "Left/Arm", "Hand\\Socket\""}, {"", "Root", "RightHand"}};
    ASSERT_TRUE(authored.Save(Registry, Model));
    const auto restored = BoneLODAssetSettings::Load(Registry, Model);
    EXPECT_EQ(restored.BoneBudget, authored.BoneBudget);
    EXPECT_EQ(restored.MandatoryPaths, authored.MandatoryPaths);
    EXPECT_TRUE(restored.Error.empty());
}

TEST_F(BoneLODAssetSettingsTest, ClearedMetadataUsesTheSameDefaultsAsAnAbsentKey)
{
    ASSERT_TRUE(Registry.SetMetaValue(Model, BoneLODAssetSettings::kMetadataKey, ""));
    const auto settings = BoneLODAssetSettings::Load(Registry, Model);
    EXPECT_EQ(settings.BoneBudget, 32u);
    EXPECT_TRUE(settings.MandatoryPaths.empty());
    EXPECT_TRUE(settings.Error.empty());
}

TEST_F(BoneLODAssetSettingsTest, MalformedMetadataDisablesReductionAndCannotOverwriteRequirements)
{
    const char* malformed[] = {"{", "[]", R"({"budget":-1})", R"({"budget":2.5})",
        R"({"budget":4294967296})", R"({"budget":"32"})", R"({"mandatoryPaths":"Hand"})",
        R"({"mandatoryPaths":[[]]})", R"({"mandatoryPaths":[["Root",9]]})", R"({"unknown":1})"};
    for (const char* metadata : malformed)
    {
        SCOPED_TRACE(metadata);
        ASSERT_TRUE(Registry.SetMetaValue(Model, BoneLODAssetSettings::kMetadataKey, metadata));
        const auto settings = BoneLODAssetSettings::Load(Registry, Model);
        EXPECT_EQ(settings.BoneBudget, 0u);
        EXPECT_FALSE(settings.Error.empty());
        EXPECT_FALSE(settings.Save(Registry, Model));
        std::string stored;
        ASSERT_TRUE(Registry.TryGetMetaValue(Model, BoneLODAssetSettings::kMetadataKey, stored));
        EXPECT_EQ(stored, metadata);
    }
}

TEST_F(BoneLODAssetSettingsTest, ZeroBudgetAndMaximumIntegerRoundTripWithoutClamping)
{
    for (uint32 budget : {0u, std::numeric_limits<uint32>::max()})
    {
        BoneLODAssetSettings authored;
        authored.BoneBudget = budget;
        ASSERT_TRUE(authored.Save(Registry, Model));
        const auto restored = BoneLODAssetSettings::Load(Registry, Model);
        EXPECT_EQ(restored.BoneBudget, budget);
        EXPECT_TRUE(restored.Error.empty());
    }
}

TEST_F(BoneLODAssetSettingsTest, EmptyPathRequirementIsRejectedWithoutChangingSavedSettings)
{
    BoneLODAssetSettings settings;
    settings.MandatoryPaths = {{"Root", "Hand"}};
    ASSERT_TRUE(settings.Save(Registry, Model));
    settings.MandatoryPaths.push_back({});
    EXPECT_FALSE(settings.Save(Registry, Model));
    EXPECT_EQ(BoneLODAssetSettings::Load(Registry, Model).MandatoryPaths,
        (std::vector<std::vector<std::string>>{{"Root", "Hand"}}));
    EXPECT_FALSE(settings.Save(Registry, {}));
}
