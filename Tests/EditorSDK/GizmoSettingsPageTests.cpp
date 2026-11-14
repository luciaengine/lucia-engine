// The Gizmo settings page's registration shape: the rows, their preference
// keys, and that Constant Screen Size picks which size rows are shown.

#include "Editor/Settings/GizmoSettingsPage.h"

#include "Editor/Settings/EditorSettingsRegistry.h"

#include <gtest/gtest.h>

#include <string>
#include <variant>
#include <vector>

namespace ed = GameEngine::Editor;

namespace
{
class GizmoSettingsPageTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        // No tool: the rows must tolerate a scene view that has none yet.
        ed::RegisterGizmoSettingsCategory([]() -> ed::SceneTools::TransformTool* { return nullptr; });
        ASSERT_TRUE(ed::EditorSettingsRegistry::Get().TryGet("gizmo", m_Category));
    }

    ed::SettingsCategoryDescriptor m_Category;
};

TEST_F(GizmoSettingsPageTest, RegistersEveryRowUnderUserSettings)
{
    // The tree sorts by title, so the title is also the page's tree position.
    EXPECT_EQ(m_Category.Title, "Gizmo");
    EXPECT_EQ(m_Category.Group, ed::SettingsCategoryGroup::UserSettings);
    EXPECT_EQ(m_Category.TreeRowClass, "gizmo-row");

    // Literal keys: the preferences users already have are stored under these.
    const std::vector<std::pair<std::string, std::string>> expected = {
        {"Constant Screen Size", "gizmo.constantScreenSize"},
        {"Enhanced Rotate Gizmo", "gizmo.rotateEnhanced"},
        {"Directional Light Details", ""},
        {"Point Light Details", ""},
        {"Spot Light Details", ""},
        {"Translate", "gizmo.translateThickness"},
        {"Rotate", "gizmo.rotateThickness"},
        {"Scale", "gizmo.scaleThickness"},
        {"Translate", "gizmo.translateScale"},
        {"Rotate", "gizmo.rotateScale"},
        {"Scale", "gizmo.scaleGizmoScale"},
        {"Translate", "gizmo.translateConstantThickness"},
        {"Rotate", "gizmo.rotateConstantThickness"},
        {"Scale", "gizmo.scaleConstantThickness"},
    };
    ASSERT_EQ(m_Category.Fields.size(), expected.size());
    for (size_t i = 0; i < expected.size(); ++i)
    {
        EXPECT_EQ(m_Category.Fields[i].Label, expected[i].first) << i;
        EXPECT_EQ(m_Category.Fields[i].PrefKey, expected[i].second) << i;
    }
    EXPECT_EQ(m_Category.Fields[5].SectionHeader, "Gizmo Thickness");
    EXPECT_EQ(m_Category.Fields[8].SectionHeader, "Gizmo Scale");
    EXPECT_EQ(m_Category.Fields[11].SectionHeader, "Gizmo Thickness");

    const auto& rotateThickness = std::get<ed::SettingsFieldDescriptor::SliderField>(m_Category.Fields[6].Control);
    EXPECT_FLOAT_EQ(rotateThickness.DefaultValue, 5.5f);
    EXPECT_FLOAT_EQ(rotateThickness.MinValue, 0.5f);
    EXPECT_FLOAT_EQ(rotateThickness.MaxValue, 10.0f);
    EXPECT_TRUE(static_cast<bool>(rotateThickness.Preview));
}

TEST_F(GizmoSettingsPageTest, ConstantScreenSizePicksTheSizeRowsShown)
{
    const auto& constantSize = std::get<ed::SettingsFieldDescriptor::ToggleField>(m_Category.Fields[0].Control);
    ASSERT_TRUE(static_cast<bool>(constantSize.Set));

    constantSize.Set(false);
    for (size_t i = 5; i < 8; ++i)
        EXPECT_TRUE(m_Category.Fields[i].IsVisible()) << "pixel thickness row " << i;
    for (size_t i = 8; i < 14; ++i)
        EXPECT_FALSE(m_Category.Fields[i].IsVisible()) << "constant-size row " << i;

    constantSize.Set(true);
    for (size_t i = 5; i < 8; ++i)
        EXPECT_FALSE(m_Category.Fields[i].IsVisible()) << "pixel thickness row " << i;
    for (size_t i = 8; i < 14; ++i)
        EXPECT_TRUE(m_Category.Fields[i].IsVisible()) << "constant-size row " << i;
}
} // namespace
