// The menu bar's heading order: File, Edit, the contributed menus, then Window last; contributed
// paths under a built-in menu join it instead of adding a second heading; separators above items.

#include <gtest/gtest.h>

#include "EditorToolbar/EditorMenuBarBuilder.h"
#include "Platform/Toolbar.h"

#include <array>
#include <string>
#include <vector>

using GameEngine::EditorMenuBarBuilder;
using GameEngine::EditorMenuPathItem;

namespace
{

// Records the headings, submenus and items in the order the builder adds them.
class RecordingToolbar final : public GameEngine::INativeToolbar
{
  public:
    struct Entry
    {
        uint32_t ParentMenuId = 0;
        std::string Title;
    };

    bool Install(GameEngine::Platform::Window*) override { return true; }
    void Uninstall() override {}
    void Clear() override {}
    void SetCommandHandler(CommandCallback) override {}
    uint32_t AddMenu(const std::string& title) override
    {
        Headings.push_back(title);
        return NextMenuId++;
    }
    uint32_t AddSubMenu(uint32_t parentMenuId, const std::string& title) override
    {
        SubMenus.push_back({parentMenuId, title});
        return NextMenuId++;
    }
    void AddItem(uint32_t parentMenuId, const std::string& title, uint32_t) override
    {
        Items.push_back({parentMenuId, title});
    }
    // Records the separator as an item with an empty title, so its place among the items shows.
    void AddSeparator(uint32_t parentMenuId) override { Items.push_back({parentMenuId, {}}); }

    std::vector<std::string> Headings;
    std::vector<Entry> SubMenus;
    std::vector<Entry> Items;
    uint32_t NextMenuId = 1;
};

TEST(EditorMenuBarBuilderTests, WindowComesAfterTheContributedMenus)
{
    RecordingToolbar toolbar;
    const std::array<EditorMenuPathItem, 3> items{{
        {"Tools/Bake Lighting", 10, ""},
        {"Window/Profiler", 11, ""},
        {"File/Export", 12, "Ctrl+E"},
    }};
    uint32_t editMenu = 0;
    uint32_t windowMenu = 0;
    uint32_t layoutMenu = 0;
    EditorMenuBarBuilder(toolbar).Build(
        items, [&](uint32_t menu) { editMenu = menu; },
        [&](uint32_t menu)
        {
            windowMenu = menu;
            layoutMenu = toolbar.AddSubMenu(menu, "Layout");
            return layoutMenu;
        });

    EXPECT_EQ(toolbar.Headings, (std::vector<std::string>{"File", "Edit", "Tools", "Window"}));
    EXPECT_EQ(editMenu, 2u);
    EXPECT_EQ(windowMenu, 4u);
    ASSERT_EQ(toolbar.Items.size(), 3u);
    EXPECT_EQ(toolbar.Items[0].ParentMenuId, 3u);
    EXPECT_EQ(toolbar.Items[0].Title, "Bake Lighting");
    EXPECT_EQ(toolbar.Items[1].ParentMenuId, 1u);
    EXPECT_EQ(toolbar.Items[1].Title, "Export\tCtrl+E");
    EXPECT_EQ(toolbar.Items[2].ParentMenuId, windowMenu);
    EXPECT_EQ(toolbar.Items[2].Title, "Profiler");
}

TEST(EditorMenuBarBuilderTests, PathsJoinTheBuiltInSubmenusAndAddMissingOnes)
{
    RecordingToolbar toolbar;
    const std::array<EditorMenuPathItem, 3> items{{
        {"Window/Layout/Compact", 20, ""},
        {"Tools//Terrain/Erode", 21, ""},
        {"NoMenu", 22, ""},
    }};
    uint32_t layoutMenu = 0;
    EditorMenuBarBuilder(toolbar).Build(
        items, [](uint32_t) {},
        [&](uint32_t menu)
        {
            layoutMenu = toolbar.AddSubMenu(menu, "Layout");
            return layoutMenu;
        });

    EXPECT_EQ(toolbar.Headings, (std::vector<std::string>{"File", "Edit", "Tools", "Window"}));
    ASSERT_EQ(toolbar.SubMenus.size(), 2u);
    EXPECT_EQ(toolbar.SubMenus[0].ParentMenuId, 3u);
    EXPECT_EQ(toolbar.SubMenus[0].Title, "Terrain");
    ASSERT_EQ(toolbar.Items.size(), 2u);
    EXPECT_EQ(toolbar.Items[0].Title, "Erode");
    EXPECT_EQ(toolbar.Items[1].ParentMenuId, layoutMenu);
    EXPECT_EQ(toolbar.Items[1].Title, "Compact");
}

TEST(EditorMenuBarBuilderTests, ContributedMenusKeepTheOrderOfTheirFirstItem)
{
    RecordingToolbar toolbar;
    const std::array<EditorMenuPathItem, 3> items{{
        {"Tools/A", 30, ""},
        {"Bake/B", 31, ""},
        {"Tools/C", 32, ""},
    }};
    EditorMenuBarBuilder(toolbar).Build(items, [](uint32_t) {}, [](uint32_t) { return 0u; });

    EXPECT_EQ(toolbar.Headings, (std::vector<std::string>{"File", "Edit", "Tools", "Bake", "Window"}));
    ASSERT_EQ(toolbar.Items.size(), 3u);
    EXPECT_EQ(toolbar.Items[0].ParentMenuId, 3u);
    EXPECT_EQ(toolbar.Items[1].ParentMenuId, 4u);
    EXPECT_EQ(toolbar.Items[2].ParentMenuId, 3u);
    EXPECT_EQ(toolbar.Items[2].Title, "C");
}

TEST(EditorMenuBarBuilderTests, ASeparatorSitsAboveItsItemInTheSameMenu)
{
    RecordingToolbar toolbar;
    const std::array<EditorMenuPathItem, 2> items{{
        {"File/Save Scene", 40, ""},
        {"File/Exit", 41, "", true},
    }};
    EditorMenuBarBuilder(toolbar).Build(items, [](uint32_t) {}, [](uint32_t) { return 0u; });

    ASSERT_EQ(toolbar.Items.size(), 3u);
    EXPECT_EQ(toolbar.Items[0].Title, "Save Scene");
    EXPECT_EQ(toolbar.Items[1].ParentMenuId, 1u);
    EXPECT_EQ(toolbar.Items[1].Title, "");
    EXPECT_EQ(toolbar.Items[2].ParentMenuId, 1u);
    EXPECT_EQ(toolbar.Items[2].Title, "Exit");
}

TEST(EditorMenuBarBuilderTests, NoContributedItemsShowsNoToolsMenu)
{
    RecordingToolbar toolbar;
    EditorMenuBarBuilder(toolbar).Build({}, [](uint32_t) {}, [](uint32_t) { return 0u; });
    EXPECT_EQ(toolbar.Headings, (std::vector<std::string>{"File", "Edit", "Window"}));
}

} // namespace
