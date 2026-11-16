#include "Editor/Hierarchy/HierarchyRowEye.h"

#include "UI/StyleProperties.h"
#include "UI/UIElement.h"

#include <gtest/gtest.h>

#include <memory>

namespace GameEngine::Editor
{
namespace
{

bool EyeHidden(const UIElement& row)
{
    for (const auto& child : row.GetChildren())
    {
        if (child && child->HasClass("hierarchy-eye"))
        {
            const auto display = child->Style().Get(Style::Display);
            return display.has_value() && *display == DisplayMode::None;
        }
    }
    ADD_FAILURE() << "the row has no eye";
    return false;
}

TEST(HierarchyRowEyeTests, TheEyeIsOnByDefaultAndTheChoiceIsKept)
{
    HierarchyRowEye first({});
    ASSERT_TRUE(first.IsShown());
    first.ToggleShown();
    EXPECT_FALSE(first.IsShown());

    HierarchyRowEye reopened({});
    EXPECT_FALSE(reopened.IsShown());
    reopened.ToggleShown();
    EXPECT_TRUE(HierarchyRowEye({}).IsShown());
}

TEST(HierarchyRowEyeTests, AnEyeTurnedOffHidesOnARowThatWouldShowIt)
{
    HierarchyRowEye eye({});
    ASSERT_TRUE(eye.IsShown());
    UIElement row;
    eye.AddTo(row);
    eye.Present(row, ECS::EntityHandle{}, true);
    EXPECT_FALSE(EyeHidden(row));

    eye.ToggleShown();
    eye.Present(row, ECS::EntityHandle{}, true);
    EXPECT_TRUE(EyeHidden(row));
    eye.ToggleShown();
}

} // namespace
} // namespace GameEngine::Editor
