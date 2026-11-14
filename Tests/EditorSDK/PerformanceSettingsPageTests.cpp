// The Performance settings page's registration shape: the rows, the preference
// keys read before the device exists, and that a VSync change reaches the
// owner the page was registered with.

#include "Editor/Settings/PerformanceSettingsPage.h"

#include "Editor/Settings/EditorSettingsRegistry.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <variant>

namespace ed = GameEngine::Editor;

TEST(PerformanceSettingsPageTest, RegistersRowsUnderUserSettings)
{
    auto appliedVsync = std::make_shared<int>(-1);
    ed::RegisterPerformanceSettingsCategory([appliedVsync](bool enabled) { *appliedVsync = enabled ? 1 : 0; });
    ed::SettingsCategoryDescriptor category;
    ASSERT_TRUE(ed::EditorSettingsRegistry::Get().TryGet("performance", category));

    // The tree sorts by title, so the title is also the page's tree position.
    EXPECT_EQ(category.Title, "Performance");
    EXPECT_EQ(category.Group, ed::SettingsCategoryGroup::UserSettings);
    EXPECT_EQ(category.TreeRowClass, "performance-row");
    ASSERT_EQ(category.Fields.size(), 3u);

    const ed::SettingsFieldDescriptor& api = category.Fields[0];
    EXPECT_EQ(api.Label, "Graphics API");
    // Read at startup to choose the backend, before any device exists.
    EXPECT_EQ(api.PrefKey, "renderer.graphicsApi");
    ASSERT_TRUE(std::holds_alternative<ed::SettingsFieldDescriptor::DropdownField>(api.Control));
    EXPECT_EQ(std::get<ed::SettingsFieldDescriptor::DropdownField>(api.Control).DefaultValue, "auto");

    const ed::SettingsFieldDescriptor& vsync = category.Fields[1];
    EXPECT_EQ(vsync.Label, "VSync");
    EXPECT_EQ(vsync.PrefKey, "performance.vsync");
    EXPECT_FALSE(vsync.InfoCard.empty());
    const auto& vsyncToggle = std::get<ed::SettingsFieldDescriptor::ToggleField>(vsync.Control);
    EXPECT_TRUE(vsyncToggle.DefaultValue);
    ASSERT_TRUE(static_cast<bool>(vsyncToggle.Set));
    vsyncToggle.Set(false);
    EXPECT_EQ(*appliedVsync, 0) << "a VSync change must reach the device owner";

    const ed::SettingsFieldDescriptor& renderScale = category.Fields[2];
    EXPECT_EQ(renderScale.Label, "Scene View Render Scale");
    // SceneViewSettings persists the render scale itself.
    EXPECT_TRUE(renderScale.PrefKey.empty());
    const auto& slider = std::get<ed::SettingsFieldDescriptor::SliderField>(renderScale.Control);
    EXPECT_FLOAT_EQ(slider.MinValue, 0.25f);
    EXPECT_FLOAT_EQ(slider.MaxValue, 2.0f);
    EXPECT_FLOAT_EQ(slider.DefaultValue, 1.0f);
    EXPECT_TRUE(static_cast<bool>(slider.Preview));

    // The running-backend card is filled in when the page is built.
    ASSERT_TRUE(static_cast<bool>(category.PrepareFields));
    ed::SettingsCategoryDescriptor built = category;
    built.PrepareFields(built);
    EXPECT_EQ(built.Fields[0].InfoCard.rfind("Currently running: ", 0), 0u) << built.Fields[0].InfoCard;
}
