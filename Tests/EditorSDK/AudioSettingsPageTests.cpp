// The Audio settings page's registration shape: the row the user sees and the
// preference key the audio inspector reads back.

#include "Editor/Settings/AudioSettingsPage.h"

#include "Editor/Settings/EditorSettingsRegistry.h"

#include <gtest/gtest.h>

#include <variant>

namespace ed = GameEngine::Editor;

TEST(AudioSettingsPageTest, RegistersAutoPlayToggleUnderProjectSettings)
{
    ed::RegisterAudioSettingsCategory();
    ed::SettingsCategoryDescriptor category;
    ASSERT_TRUE(ed::EditorSettingsRegistry::Get().TryGet("audio", category));

    EXPECT_EQ(category.Title, "Audio Settings");
    EXPECT_EQ(category.Group, ed::SettingsCategoryGroup::ProjectSettings);
    EXPECT_EQ(category.TreeRowClass, "audio-settings-row");

    ASSERT_EQ(category.Fields.size(), 1u);
    const ed::SettingsFieldDescriptor& autoPlay = category.Fields.front();
    EXPECT_EQ(autoPlay.Label, "Auto-play On Select");
    // AudioInspector reads this literal key; renaming it drops the user's choice.
    EXPECT_EQ(autoPlay.PrefKey, "audio.autoPlayOnSelection");
    ASSERT_TRUE(std::holds_alternative<ed::SettingsFieldDescriptor::ToggleField>(autoPlay.Control));
    EXPECT_TRUE(std::get<ed::SettingsFieldDescriptor::ToggleField>(autoPlay.Control).DefaultValue);
}
