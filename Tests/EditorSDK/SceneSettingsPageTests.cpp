// The Scene settings page's registration shape: the auto-save rows and the
// preference keys SceneEditorController reads back.

#include "Editor/Settings/SceneSettingsPage.h"

#include "Editor/Settings/EditorSettingsRegistry.h"

#include <gtest/gtest.h>

#include <variant>

namespace ed = GameEngine::Editor;

TEST(SceneSettingsPageTest, RegistersAutoSaveRowsUnderProjectSettings)
{
    ed::RegisterSceneSettingsCategory();
    ed::SettingsCategoryDescriptor category;
    ASSERT_TRUE(ed::EditorSettingsRegistry::Get().TryGet("scene", category));

    EXPECT_EQ(category.Title, "Scene");
    EXPECT_EQ(category.Group, ed::SettingsCategoryGroup::ProjectSettings);
    EXPECT_EQ(category.TreeRowClass, "scene-row");
    EXPECT_FALSE(category.Description.empty());

    ASSERT_EQ(category.Fields.size(), 2u);

    const ed::SettingsFieldDescriptor& enabled = category.Fields[0];
    EXPECT_EQ(enabled.Label, "Auto-save Backup Copy");
    EXPECT_EQ(enabled.SectionHeader, "Auto-save");
    // SceneEditorController reads these literal keys; renaming one drops the user's choice.
    EXPECT_EQ(enabled.PrefKey, "editor.autoSaveEnabled");
    ASSERT_TRUE(std::holds_alternative<ed::SettingsFieldDescriptor::ToggleField>(enabled.Control));
    EXPECT_TRUE(std::get<ed::SettingsFieldDescriptor::ToggleField>(enabled.Control).DefaultValue);

    const ed::SettingsFieldDescriptor& interval = category.Fields[1];
    EXPECT_EQ(interval.Label, "Interval (seconds)");
    EXPECT_EQ(interval.PrefKey, "editor.autoSaveIntervalSeconds");
    ASSERT_TRUE(std::holds_alternative<ed::SettingsFieldDescriptor::SliderField>(interval.Control));
    const auto& slider = std::get<ed::SettingsFieldDescriptor::SliderField>(interval.Control);
    // The slider range is the controller's clamp range: a narrower slider would
    // clamp a stored value on page open and write it back.
    EXPECT_FLOAT_EQ(slider.MinValue, 30.0f);
    EXPECT_FLOAT_EQ(slider.MaxValue, 300.0f);
    EXPECT_FLOAT_EQ(slider.DefaultValue, 120.0f);
}
