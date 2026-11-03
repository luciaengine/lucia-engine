// The Input settings page's registration shape: the Scene View mouse rows and
// the range SceneViewSettings clamps look sensitivity to.

#include "Editor/Settings/InputSettingsPage.h"

#include "Editor/Settings/EditorSettingsRegistry.h"

#include <gtest/gtest.h>

#include <variant>

namespace ed = GameEngine::Editor;

TEST(InputSettingsPageTest, RegistersSceneViewMouseRowsUnderProjectSettings)
{
    ed::RegisterInputSettingsCategory();
    ed::SettingsCategoryDescriptor category;
    ASSERT_TRUE(ed::EditorSettingsRegistry::Get().TryGet("input", category));

    // The tree sorts by title, so the title is also the page's tree position.
    EXPECT_EQ(category.Title, "Input");
    EXPECT_EQ(category.Group, ed::SettingsCategoryGroup::ProjectSettings);
    EXPECT_EQ(category.TreeRowClass, "input-row");

    ASSERT_EQ(category.Fields.size(), 2u);

    const ed::SettingsFieldDescriptor& sensitivity = category.Fields[0];
    EXPECT_EQ(sensitivity.Label, "Look Sensitivity");
    EXPECT_EQ(sensitivity.SectionHeader, "Scene View Mouse");
    // SceneViewSettings persists these rows itself; a PrefKey would write a
    // second copy of the value under a key nothing reads.
    EXPECT_TRUE(sensitivity.PrefKey.empty());
    ASSERT_TRUE(std::holds_alternative<ed::SettingsFieldDescriptor::SliderField>(sensitivity.Control));
    const auto& slider = std::get<ed::SettingsFieldDescriptor::SliderField>(sensitivity.Control);
    // Literal values: the clamp range in SceneViewSettings::Validate.
    EXPECT_FLOAT_EQ(slider.MinValue, 0.01f);
    EXPECT_FLOAT_EQ(slider.MaxValue, 2.0f);
    EXPECT_FLOAT_EQ(slider.DefaultValue, 0.2f);
    EXPECT_TRUE(static_cast<bool>(slider.Get));
    EXPECT_TRUE(static_cast<bool>(slider.Set));

    const ed::SettingsFieldDescriptor& invertY = category.Fields[1];
    EXPECT_EQ(invertY.Label, "Invert Y Axis");
    EXPECT_TRUE(invertY.PrefKey.empty());
    ASSERT_TRUE(std::holds_alternative<ed::SettingsFieldDescriptor::ToggleField>(invertY.Control));
    EXPECT_FALSE(std::get<ed::SettingsFieldDescriptor::ToggleField>(invertY.Control).DefaultValue);
}
