// The Script settings page's registration shape: the rows, the line-height
// range ScriptEditorSettings clamps to, and the syntax colour rows.

#include "Editor/Settings/ScriptSettingsPage.h"

#include "Editor/Settings/EditorSettingsRegistry.h"

#include <gtest/gtest.h>

#include <string>
#include <variant>
#include <vector>

namespace ed = GameEngine::Editor;

TEST(ScriptSettingsPageTest, RegistersRowsUnderUserSettings)
{
    ed::RegisterScriptSettingsCategory();
    ed::SettingsCategoryDescriptor category;
    ASSERT_TRUE(ed::EditorSettingsRegistry::Get().TryGet("script", category));

    // The tree sorts by title, so the title is also the page's tree position.
    EXPECT_EQ(category.Title, "Script");
    EXPECT_EQ(category.Group, ed::SettingsCategoryGroup::UserSettings);
    EXPECT_EQ(category.TreeRowClass, "script-row");

    const std::vector<std::string> expectedLabels = {
        "Open in Script Inspector", ".glsl Opens Material Graph", "Line Height", "Default Text",
        "Keywords",                 "Strings",                    "Comments",    "Numbers",
        "Types"};
    ASSERT_EQ(category.Fields.size(), expectedLabels.size());
    for (size_t i = 0; i < expectedLabels.size(); ++i)
        EXPECT_EQ(category.Fields[i].Label, expectedLabels[i]);
    EXPECT_EQ(category.Fields[0].SectionHeader, "Script Editor");
    EXPECT_EQ(category.Fields[3].SectionHeader, "Syntax Highlighting");
    EXPECT_FALSE(category.Fields[0].InfoCard.empty());
    EXPECT_FALSE(category.Fields[1].InfoCard.empty());

    // Literal values: ScriptEditorSettings' clamp range and default.
    const auto& lineHeight = std::get<ed::SettingsFieldDescriptor::SliderField>(category.Fields[2].Control);
    EXPECT_FLOAT_EQ(lineHeight.MinValue, 0.5f);
    EXPECT_FLOAT_EQ(lineHeight.MaxValue, 4.0f);
    EXPECT_FLOAT_EQ(lineHeight.DefaultValue, 1.0f);

    for (size_t i = 3; i < category.Fields.size(); ++i)
    {
        const auto* color = std::get_if<ed::SettingsFieldDescriptor::ColorField>(&category.Fields[i].Control);
        ASSERT_NE(color, nullptr) << category.Fields[i].Label;
        EXPECT_TRUE(color->ShowHexField) << category.Fields[i].Label;
        // Picker changes preview without saving; only the apply saves.
        EXPECT_TRUE(static_cast<bool>(color->Preview)) << category.Fields[i].Label;
    }
    EXPECT_EQ(std::get<ed::SettingsFieldDescriptor::ColorField>(category.Fields[4].Control).DefaultArgb, 0xFFBFBFBFu);
}
