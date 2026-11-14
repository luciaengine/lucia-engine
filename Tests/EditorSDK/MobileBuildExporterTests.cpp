#include "Editor/BuildExporters/MobileBuildExporter.h"
#include "Editor/Registries/BuildExporterRegistry.h"
#include "Editor/Settings/EditorSettingsRegistry.h"
#include <gtest/gtest.h>

TEST(MobileBuildExporterTests, RegistersBothExportersAndProjectSettings)
{
    using namespace GameEngine::Editor;
    RegisterMobileBuildExporters();
    for (const std::string platform : {"Android", "iOS"})
    {
        BuildExporterDescriptor exporter;
        ASSERT_TRUE(BuildExporterRegistry::Get().TryFindForPlatform(platform, exporter));
        EXPECT_EQ(exporter.ExporterId, "mobile." + platform);
        SettingsCategoryDescriptor category;
        ASSERT_TRUE(EditorSettingsRegistry::Get().TryGet("build." + platform + ".mobile", category));
        EXPECT_EQ(category.ParentCategoryId, "build." + platform);
        EXPECT_EQ(category.Group, SettingsCategoryGroup::ProjectSettings);
        EXPECT_EQ(category.Fields.size(), 4u);
        EXPECT_EQ(category.Fields.front().Label, "Engine checkout");
    }
}
