#include "EditorToolbar/EditorMenuBarBuilder.h"

#include "Platform/Toolbar.h"
#include "Types/StringUtils.h"

#include <string_view>
#include <vector>

namespace GameEngine
{
namespace
{
constexpr std::string_view kWindowMenu = "Window";

// The non-empty segments of a menu path: "Tools//Bake" gives "Tools" and "Bake".
std::vector<std::string_view> PathSegments(std::string_view path)
{
    std::vector<std::string_view> segments = Split(path, '/');
    std::erase(segments, std::string_view());
    return segments;
}

bool IsUnderWindowMenu(std::string_view path)
{
    const std::vector<std::string_view> segments = PathSegments(path);
    return !segments.empty() && segments.front() == kWindowMenu;
}
} // namespace

EditorMenuBarBuilder::EditorMenuBarBuilder(INativeToolbar& toolbar)
    : m_Toolbar(toolbar)
{
}

void EditorMenuBarBuilder::Build(std::span<const EditorMenuPathItem> items, FillEditMenu fillEdit,
                                 FillWindowMenu fillWindow)
{
    m_MenuIds = {};
    m_MenuIds.InsertOrAssign("File", m_Toolbar.AddMenu("File"));
    const uint32_t editMenu = m_Toolbar.AddMenu("Edit");
    m_MenuIds.InsertOrAssign("Edit", editMenu);
    fillEdit(editMenu);

    for (const EditorMenuPathItem& item : items)
    {
        if (!IsUnderWindowMenu(item.Path))
            AddPathItem(item);
    }

    const uint32_t windowMenu = m_Toolbar.AddMenu(std::string(kWindowMenu));
    m_MenuIds.InsertOrAssign(std::string(kWindowMenu), windowMenu);
    m_MenuIds.InsertOrAssign("Window/Layout", fillWindow(windowMenu));
    for (const EditorMenuPathItem& item : items)
    {
        if (IsUnderWindowMenu(item.Path))
            AddPathItem(item);
    }
}

void EditorMenuBarBuilder::AddPathItem(const EditorMenuPathItem& item)
{
    const std::vector<std::string_view> segments = PathSegments(item.Path);
    // An item needs a menu and a title.
    if (segments.size() < 2)
        return;

    std::string prefix;
    uint32_t parentMenuId = 0;
    for (std::size_t i = 0; i + 1 < segments.size(); ++i)
    {
        if (!prefix.empty())
            prefix += '/';
        prefix += segments[i];
        if (const uint32_t* found = m_MenuIds.Find(prefix))
        {
            parentMenuId = *found;
            continue;
        }
        const std::string title(segments[i]);
        parentMenuId = parentMenuId == 0 ? m_Toolbar.AddMenu(title) : m_Toolbar.AddSubMenu(parentMenuId, title);
        m_MenuIds.InsertOrAssign(prefix, parentMenuId);
    }

    if (item.SeparatorBefore)
        m_Toolbar.AddSeparator(parentMenuId);
    std::string title(segments.back());
    if (!item.Shortcut.empty())
    {
        title += '\t';
        title += item.Shortcut;
    }
    m_Toolbar.AddItem(parentMenuId, title, item.CommandId);
}
} // namespace GameEngine
