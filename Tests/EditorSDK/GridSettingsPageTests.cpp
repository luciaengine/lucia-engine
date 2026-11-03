// The Grid & Snapping settings page's registration shape: the rows, the snap
// preference key the scene view reads at startup, and that a committed snap
// size reaches the owner the page was registered with.

#include "Editor/Settings/GridSettingsPage.h"

#include "Editor/Settings/EditorSettingsRegistry.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace ed = GameEngine::Editor;

TEST(GridSettingsPageTest, RegistersGridRowsUnderUserSettings)
{
    auto appliedSnapSize = std::make_shared<float>(0.0f);
    ed::RegisterGridSettingsCategory([appliedSnapSize](float size) { *appliedSnapSize = size; });
    ed::SettingsCategoryDescriptor category;
    ASSERT_TRUE(ed::EditorSettingsRegistry::Get().TryGet("gridSnapping", category));

    // The tree sorts by title, so the title is also the page's tree position.
    EXPECT_EQ(category.Title, "Grid & Snapping");
    EXPECT_EQ(category.Group, ed::SettingsCategoryGroup::UserSettings);
    EXPECT_EQ(category.TreeRowClass, "grid-snapping-row");

    const std::vector<std::string> expectedLabels = {"Show Grid on Startup", "Grid Color (3D)",
                                                     "Grid Color (2D)",      "Opacity (3D)",
                                                     "Opacity (2D)",         "Snap Size"};
    ASSERT_EQ(category.Fields.size(), expectedLabels.size());
    for (size_t i = 0; i < expectedLabels.size(); ++i)
        EXPECT_EQ(category.Fields[i].Label, expectedLabels[i]);

    EXPECT_TRUE(std::holds_alternative<ed::SettingsFieldDescriptor::ToggleField>(category.Fields[0].Control));
    const auto& color2D = std::get<ed::SettingsFieldDescriptor::ColorField>(category.Fields[2].Control);
    EXPECT_EQ(color2D.DefaultArgb, 0x7AFFFFFFu);
    // The picker previews the grid colours without saving on every change.
    for (size_t i : {1u, 2u})
        EXPECT_TRUE(static_cast<bool>(std::get<ed::SettingsFieldDescriptor::ColorField>(category.Fields[i].Control).Preview))
            << category.Fields[i].Label;

    // The opacity sliders follow the thumb without saving on every tick.
    for (size_t i : {3u, 4u})
    {
        const auto& opacity = std::get<ed::SettingsFieldDescriptor::SliderField>(category.Fields[i].Control);
        EXPECT_FLOAT_EQ(opacity.MinValue, 0.0f);
        EXPECT_FLOAT_EQ(opacity.MaxValue, 1.0f);
        EXPECT_TRUE(static_cast<bool>(opacity.Preview)) << category.Fields[i].Label;
    }

    const ed::SettingsFieldDescriptor& snap = category.Fields[5];
    // GizmoSettingsWiring reads this key to seed the scene view at startup.
    EXPECT_EQ(snap.PrefKey, "grid.snapSize");
    const auto& snapSlider = std::get<ed::SettingsFieldDescriptor::SliderField>(snap.Control);
    EXPECT_FLOAT_EQ(snapSlider.DefaultValue, 0.5f);
    EXPECT_FLOAT_EQ(snapSlider.MinValue, 0.1f);
    EXPECT_FLOAT_EQ(snapSlider.MaxValue, 10.0f);
    ASSERT_TRUE(static_cast<bool>(snapSlider.Set));
    snapSlider.Set(2.5f);
    EXPECT_FLOAT_EQ(*appliedSnapSize, 2.5f) << "a committed snap size must reach the scene view";
}
