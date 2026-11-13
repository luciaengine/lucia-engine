#pragma once

#include "Types/FlatMap.h"
#include "Types/FunctionRef.h"

#include <cstdint>
#include <span>
#include <string>

namespace GameEngine
{
class INativeToolbar;

/// A contributed menu item: `Path` is "Menu/Item" or "Menu/Submenu/.../Item".
struct EditorMenuPathItem
{
    std::string Path;
    uint32_t CommandId = 0;
    /// Shown after the title, after a tab. May be empty.
    std::string Shortcut;
    /// Draws a separator line above the item in its menu.
    bool SeparatorBefore = false;
};

/// Builds the menu bar in a fixed heading order: File, Edit, the contributed menus in the order of
/// their first item, then Window. A contributed path under a built-in menu joins that menu. A menu or
/// submenu is added with its first item, so a menu with no items never shows.
class EditorMenuBarBuilder
{
  public:
    /// The editor's own items of Edit; the argument is the menu id.
    using FillEditMenu = FunctionRef<void(uint32_t)>;
    /// The editor's own items of Window; the argument is the menu id, the result the Layout submenu id.
    using FillWindowMenu = FunctionRef<uint32_t(uint32_t)>;

    explicit EditorMenuBarBuilder(INativeToolbar& toolbar);

    /// Adds every menu and item. `items` are in display order within each menu.
    void Build(std::span<const EditorMenuPathItem> items, FillEditMenu fillEdit, FillWindowMenu fillWindow);

  private:
    void AddPathItem(const EditorMenuPathItem& item);

    INativeToolbar& m_Toolbar;
    // Menu ids by path prefix: "Tools", "Tools/Bake".
    FlatMap<std::string, uint32_t> m_MenuIds;
};
} // namespace GameEngine
