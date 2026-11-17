#include "Editor/Hierarchy/HierarchyRowEye.h"

#include "Components/Hierarchy.h"
#include "Components/Transform.h"
#include "ECS/DisabledInHierarchySystem.h"
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "Editor/Hierarchy/HierarchyEnableState.h"
#include "Editor/Hierarchy/HierarchyRowActivity.h"
#include "Editor/Settings/EditorSettingsRegistry.h"
#include "Editor/Settings/SettingsStore.h"
#include "UI/StyleProperties.h"
#include "UI/UIElement.h"

#include <gtest/gtest.h>

#include <memory>
#include <variant>

namespace GameEngine::Editor
{
namespace
{

UIElement* ChildWithClass(UIElement& row, const char* className)
{
    for (const auto& child : row.GetChildren())
    {
        if (child && child->HasClass(className))
            return child.get();
    }
    return nullptr;
}

bool Hidden(const UIElement& element)
{
    const auto display = element.Style().Get(Style::Display);
    return display.has_value() && *display == DisplayMode::None;
}

// A row as the Hierarchy builds it: the title, the entity icon and the eye.
struct EyeRow
{
    ECS::World World;
    ECS::DisabledInHierarchySystem Pass;
    HierarchyRowActivity Activity;
    std::unique_ptr<HierarchyRowEye> Eye;
    UIElement Row;
    ECS::EntityHandle Entity;

    EyeRow()
    {
        Eye = std::make_unique<HierarchyRowEye>();
        auto title = std::make_unique<UIElement>();
        title->AddClass("tree-title");
        Row.AddChild(std::move(title));
        auto icon = std::make_unique<UIElement>();
        icon->AddClass("hierarchy-preview-icon");
        Row.AddChild(std::move(icon));
        Eye->AddTo(Row);
        Entity = World.CreateEntity();
        World.AddComponentImmediate<Components::Transform>(Entity, Components::Transform{});
    }

    // What the Hierarchy's icon press does on either icon, then the row shown again.
    void Toggle()
    {
        const ECS::EntityHandle targets[] = {Entity};
        CommitEntityEnabledToggle(World, nullptr, nullptr, targets, !ECS::Entity(&World, Entity).IsEnabled());
        Show();
    }

    void Show()
    {
        Pass.Update(World, 0.0f);
        Activity.Present(World, Entity, 1, Row);
        Eye->Present(Row, Entity);
    }

    bool EyeClosed() { return ChildWithClass(Row, "hierarchy-eye")->HasClass("eye-closed"); }
    bool IconOff() { return ChildWithClass(Row, "hierarchy-preview-icon")->HasClass("hierarchy-preview-icon-off"); }
};

TEST(HierarchyRowEyeTests, TheEyeIsAnIconOfTheTreeAndShowsTheEntityIconsState)
{
    EyeRow row;
    row.Show();
    EXPECT_TRUE(ChildWithClass(row.Row, "hierarchy-eye")->HasClass("tree-icon-hit-target"))
        << "the tree's icon press, selection and drag reach the eye";
    EXPECT_FALSE(row.EyeClosed());
    EXPECT_FALSE(row.IconOff());

    row.Toggle();
    EXPECT_TRUE(row.EyeClosed());
    EXPECT_TRUE(row.IconOff());
    row.Toggle();
    EXPECT_FALSE(row.EyeClosed());
    EXPECT_FALSE(row.IconOff());
}

TEST(HierarchyRowEyeTests, AnEntityInactiveUnderASwitchedOffParentHasADimmedEye)
{
    EyeRow row;
    const ECS::EntityHandle parent = row.World.CreateEntity();
    row.World.AddComponentImmediate<Components::Transform>(parent, Components::Transform{});
    row.World.AddComponentImmediate<Components::Parent>(row.Entity, Components::Parent{parent});
    ECS::Entity(&row.World, parent).SetEnabled(false);
    row.Show();
    UIElement* eye = ChildWithClass(row.Row, "hierarchy-eye");
    EXPECT_TRUE(eye->HasClass("eye-closed-by-parent"));
    EXPECT_FALSE(eye->HasClass("eye-closed")) << "the entity's own switch is on";
    EXPECT_FALSE(row.IconOff());
}

TEST(HierarchyRowEyeTests, OneStoredChoiceServesTheMenuAndTheSettingsPage)
{
    EyeRow row;
    ASSERT_TRUE(IsHierarchyEyeShown()) << "the eye column is on by default";
    EXPECT_FALSE(row.Eye->ConsumeShownChange());
    row.Show();
    EXPECT_FALSE(Hidden(*ChildWithClass(row.Row, "hierarchy-eye")));

    RegisterHierarchySettingsCategory();
    SettingsCategoryDescriptor page;
    ASSERT_TRUE(EditorSettingsRegistry::Get().TryGet("hierarchy", page));
    ASSERT_EQ(page.Fields.size(), 1u);
    const auto* toggle = std::get_if<SettingsFieldDescriptor::ToggleField>(&page.Fields[0].Control);
    ASSERT_NE(toggle, nullptr);

    toggle->Set(false); // the Settings page
    EXPECT_FALSE(IsHierarchyEyeShown());
    EXPECT_FALSE(toggle->Get()) << "the menu item reads the same choice";
    EXPECT_TRUE(row.Eye->ConsumeShownChange()) << "an open Hierarchy restyles its rows once";
    EXPECT_FALSE(row.Eye->ConsumeShownChange());
    row.Show();
    EXPECT_TRUE(Hidden(*ChildWithClass(row.Row, "hierarchy-eye")));
    bool stored = true;
    auto preferences = OpenEditorPreferences();
    ASSERT_TRUE(preferences.Load(nullptr));
    ASSERT_TRUE(preferences.TryGetBool("hierarchy.showSceneViewEye", stored));
    EXPECT_FALSE(stored);

    SetHierarchyEyeShown(true); // the Hierarchy Options menu item
    EXPECT_TRUE(toggle->Get());
}

} // namespace
} // namespace GameEngine::Editor
