// The Asset Import settings page: its registration shape, that opening it does
// not write the project file, and that a toggle writes its own FBX key.

#include "Editor/Settings/AssetImportSettingsPage.h"

#include "Core/Engine.h"
#include "Editor/Settings/EditorSettingsRegistry.h"
#include "Editor/Settings/FbxImportSettings.h"

#include "../TestTempDir.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <variant>
#include <vector>

namespace
{
namespace ed = GameEngine::Editor;
namespace fs = std::filesystem;

constexpr const char* kSeedProjectSettings = R"({
  "schemaVersion": 1,
  "assets.fbx.importCameras": false
})";

std::string ReadFile(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

class AssetImportSettingsPageTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_WorkspaceRoot = GameEngine::TestUtils::MakeUniqueTempDirectory("ge_asset_import_settings_page");
        fs::create_directories(m_WorkspaceRoot / ".Editor");
        {
            std::ofstream out(SettingsFile(), std::ios::binary | std::ios::trunc);
            out << kSeedProjectSettings;
        }
        GameEngine::EngineCore::GetInstance().SetWorkspaceRoot(m_WorkspaceRoot);

        ed::RegisterAssetImportSettingsCategory();
        ASSERT_TRUE(ed::EditorSettingsRegistry::Get().TryGet("assetImport", m_Category));
    }

    void TearDown() override
    {
        std::error_code ec;
        fs::remove_all(m_WorkspaceRoot, ec);
    }

    fs::path SettingsFile() const { return m_WorkspaceRoot / ".Editor" / "ProjectSettings.json"; }

    fs::path m_WorkspaceRoot;
    ed::SettingsCategoryDescriptor m_Category;
};

TEST_F(AssetImportSettingsPageTest, RegistersRowsUnderProjectSettings)
{
    // The tree sorts by title, so the title is also the page's tree position.
    EXPECT_EQ(m_Category.Title, "Asset Import");
    EXPECT_EQ(m_Category.Group, ed::SettingsCategoryGroup::ProjectSettings);
    EXPECT_EQ(m_Category.TreeRowClass, "asset-import-row");

    const std::vector<std::string> expectedLabels = {
        "Default Raster Size",      "Scene Extras",       "Cameras",
        "Lights",                   "Helper Nodes",       "Generate Missing Tangents",
        "Clean Skin Weights",       "Adjust Pivots",      "Preserve Geo Transforms",
        "Import Embedded Textures", "Auto-Generate LODs"};
    ASSERT_EQ(m_Category.Fields.size(), expectedLabels.size());
    for (size_t i = 0; i < expectedLabels.size(); ++i)
        EXPECT_EQ(m_Category.Fields[i].Label, expectedLabels[i]);
    EXPECT_EQ(m_Category.Fields[0].SectionHeader, "SVG Texture Import");
    EXPECT_EQ(m_Category.Fields[1].SectionHeader, "FBX Import");
    EXPECT_EQ(m_Category.Fields[10].SectionHeader, "Automatic LODs");

    const auto& svgSize = std::get<ed::SettingsFieldDescriptor::DropdownField>(m_Category.Fields[0].Control);
    EXPECT_EQ(svgSize.DefaultValue, "256");
    const auto options = svgSize.OptionsProvider();
    ASSERT_FALSE(options.empty());
    EXPECT_EQ(options.front().Value, "16");
    EXPECT_EQ(options.back().Value, "16384");

    // Project settings, not editor preferences: no row may mirror into
    // Preferences.json.
    for (const auto& field : m_Category.Fields)
        EXPECT_TRUE(field.PrefKey.empty()) << field.Label;
}

TEST_F(AssetImportSettingsPageTest, OpeningThePageLeavesTheProjectFileUntouched)
{
    const std::string before = ReadFile(SettingsFile());

    // What the toggle rows do when the page is built.
    for (const auto& field : m_Category.Fields)
    {
        if (const auto* toggle = std::get_if<ed::SettingsFieldDescriptor::ToggleField>(&field.Control))
            toggle->Set(toggle->Get());
    }

    EXPECT_EQ(ReadFile(SettingsFile()), before) << "opening Asset Import rewrote the project file";
}

TEST_F(AssetImportSettingsPageTest, ATogglePersistsItsOwnFbxKey)
{
    const auto& cameras = std::get<ed::SettingsFieldDescriptor::ToggleField>(m_Category.Fields[2].Control);
    EXPECT_FALSE(cameras.Get()) << "the row must read the stored value";

    cameras.Set(true);
    EXPECT_TRUE(ed::FbxImportSettings::Load(m_WorkspaceRoot).ImportCameras);
    EXPECT_TRUE(cameras.Get());
}
} // namespace
