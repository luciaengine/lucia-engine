#include "EditorToolbar/EditorToolbar.h"
#include "EditorToolbar/EditorMenuBarBuilder.h"
#include "Core/Engine.h"
#include "Editor/Registries/EditorMenuRegistry.h"
#include "Editor/Shortcuts/EditorShortcuts.h"
#include "EditorApplication/EditorApplication.h"
#include "EditorContextMenu/UIContextMenu.h"
#include "EditorPanelManager.h"
#include "Platform/Toolbar.h"
#include "Platform/Capabilities.h"
#include "Platform/Window.h"
#include "UI/EditorIcons.h"
#include "UI/Controls/EditorTitleBar.h"
#include "Scripting/EditorScriptMenuRegistry.h"

#include <algorithm>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace GameEngine
{
namespace
{
constexpr uint32_t kCmdWindowLayoutReset = 0x2001;
constexpr uint32_t kCmdEditUndo = 0x1001;
constexpr uint32_t kCmdEditRedo = 0x1002;

// Generate this menu from EditorPanelManager's live inventory instead of a
// hard-coded panel table. This includes package panels and keeps titles and icons
// consistent with the top-toolbar burger menu.
// Range 0x3000–0x30FF is reserved for the generated native-menu commands.
constexpr uint32_t kWindowPanelCommandFirst = 0x3001;
constexpr uint32_t kWindowPanelCommandLast = 0x30FF;

static void BuildUndoRedoTitles(EditorApplication* app, std::string& undoTitle, std::string& redoTitle)
{
    undoTitle = "Undo";
    if (app && app->CanUndo())
    {
        if (const char* name = app->GetUndoActionName())
        {
            undoTitle += " ";
            undoTitle += name;
        }
    }
    const bool commandModifier = Platform::PrimaryShortcutModifierIsCommand();
    undoTitle += commandModifier ? "\tCmd+Z" : "\tCtrl+Z";

    redoTitle = "Redo";
    if (app && app->CanRedo())
    {
        if (const char* name = app->GetRedoActionName())
        {
            redoTitle += " ";
            redoTitle += name;
        }
    }
    redoTitle += commandModifier ? "\tCmd+Shift+Z" : "\tCtrl+Y";
}
} // namespace

EditorToolbar::~EditorToolbar()
{
    Uninstall();
}

bool EditorToolbar::Install(Platform::Window* window, EditorApplication* app)
{
    if (!window || !app)
        return false;
    m_Toolbar = CreateNativeToolbar();
    if (!m_Toolbar)
        return false;
    m_Toolbar->SetMenuFactory([] { return CreateContextMenu(); });
    if (!m_Toolbar->Install(window))
    {
        m_Toolbar.reset();
        return false;
    }

    m_App = app;
    m_Window = window;
    m_ExitItem = Editor::EditorMenuRegistry::Get().RegisterMenuItem({
        .Path = "File/Exit",
        .Priority = 4,
        .Action = [app]() {
            if (!app->TryInterceptQuitForDirtyScene())
                app->RequestExit();
        },
        .SeparatorBefore = true});
    m_ShortcutBindingsSaved = Editor::ShortcutBindingsSaved().Subscribe(
        [this](const Editor::ShortcutCatalogEntry&, const std::vector<Editor::ShortcutBinding>&) { m_RefreshPending = true; });
    m_Toolbar->SetCommandHandler([this](uint32_t cmd)
                                 {
        if (!m_App)
            return;
        if (cmd == kCmdWindowLayoutReset) m_App->ResetLayoutToDefault();
        else if (cmd == kCmdEditUndo) m_App->Undo();
        else if (cmd == kCmdEditRedo) m_App->Redo();
        else if (const auto panel = m_WindowPanelCommands.find(cmd);
                 panel != m_WindowPanelCommands.end())
        {
            m_App->OpenPanel(panel->second);
        }
        else if (Editor::EditorMenuRegistry::Get().TryInvoke(cmd))
        {
            // Native package/module menu item — dispatched by the registry.
        }
        else
        {
            // Script-driven toolbar items (Editor-managed snapshot)
            uint64_t dom = 0;
            std::string method;
            if (Editor::ScriptMenuRegistry::Get().TryResolveCommand(cmd, dom, method) && !method.empty())
            {
                try
                {
                    auto& eng = EngineCore::GetInstance();
                    auto& clr = eng.GetScriptManager().GetCLRHost();
                    int32_t out = 0;
                    // Domain 0 is treated as \"current\" by managed routing; prefer snapshot domain when provided.
                    (void)clr.InvokeInDomain(dom, method.c_str(), (uint32_t)method.size(), &out);
                }
                catch (...)
                {
                    // best-effort; keep menu handler exception-free
                }
            }
        } });

    Build(app);
    return true;
}

void EditorToolbar::Uninstall()
{
    if (auto* titleBar = m_TitleBar.Get())
        titleBar->Bind(nullptr, nullptr);
    m_TitleBar = {};
    m_ExitItem.Reset();
    m_ShortcutBindingsSaved.Reset();
    m_Window = nullptr;
    m_RefreshPending = false;
    if (m_Toolbar)
    {
        m_Toolbar->Uninstall();
        m_Toolbar.reset();
    }
    m_App = nullptr;
    m_PanelManager = nullptr;
}

void EditorToolbar::Clear()
{
    if (m_Toolbar)
        m_Toolbar->Clear();
}

void EditorToolbar::Refresh()
{
    if (!m_Toolbar || !m_App)
        return;
    if (m_Toolbar->GetOpenMenu())
    {
        m_RefreshPending = true;
        return;
    }
    m_RefreshPending = false;
    Build(m_App);
}

void EditorToolbar::AttachToUI(UIElement* root)
{
    if (!root || !m_Toolbar || m_Toolbar->GetPresentation() == ToolbarPresentation::NativeMenuBar)
        return;
    // layout.uxml declares the title bar as the root's first child.
    m_TitleBar = root->FindById<EditorTitleBar>("EditorTitleBar"_sid);
    if (auto* titleBar = m_TitleBar.Get())
        titleBar->Bind(m_Toolbar.get(), m_Window);
}

void EditorToolbar::Update(UIElement* root)
{
    // Consume both flags even when one already asks for the rebuild.
    const bool scriptDirty = Editor::ScriptMenuRegistry::Get().ConsumeToolbarDirty();
    const bool nativeDirty = Editor::EditorMenuRegistry::Get().ConsumeDirty();
    if (m_RefreshPending || scriptDirty || nativeDirty)
        Refresh();
    if (!m_TitleBar.Get() && root)
        AttachToUI(root);
    if (auto* titleBar = m_TitleBar.Get())
        titleBar->UpdateState(m_Caption);
}

void EditorToolbar::UpdateUndoRedoTitles()
{
    if (!m_Toolbar || !m_App)
        return;

    std::string undoTitle;
    std::string redoTitle;
    BuildUndoRedoTitles(m_App, undoTitle, redoTitle);

    m_Toolbar->UpdateItemTitle(kCmdEditUndo, undoTitle);
    m_Toolbar->UpdateItemTitle(kCmdEditRedo, redoTitle);
}

uint32_t EditorToolbar::AddMenu(const std::string& title)
{
    return m_Toolbar ? m_Toolbar->AddMenu(title) : 0;
}

uint32_t EditorToolbar::AddSubMenu(uint32_t parentMenuId, const std::string& title)
{
    return m_Toolbar ? m_Toolbar->AddSubMenu(parentMenuId, title) : 0;
}

void EditorToolbar::AddItem(uint32_t parentMenuId, const std::string& title, uint32_t commandId)
{
    if (m_Toolbar)
        m_Toolbar->AddItem(parentMenuId, title, commandId);
}

void EditorToolbar::SetItemIcon(uint32_t commandId, const std::string& imagePath)
{
    if (m_Toolbar)
        m_Toolbar->SetItemIcon(commandId, imagePath);
}

void EditorToolbar::Build(EditorApplication* app)
{
    Clear();
    m_WindowPanelCommands.clear();
    if (!m_Toolbar)
        return;

    const std::vector<EditorMenuPathItem> items = CollectContributedMenuItems();
    EditorMenuBarBuilder builder(*m_Toolbar);
    builder.Build(
        items, [this, app](uint32_t editMenu) { FillEditMenu(app, editMenu); },
        [this](uint32_t windowMenu) { return FillWindowMenu(windowMenu); });
}

void EditorToolbar::FillEditMenu(EditorApplication* app, uint32_t editMenu)
{
    std::string undoTitle;
    std::string redoTitle;
    BuildUndoRedoTitles(app, undoTitle, redoTitle);
    AddItem(editMenu, undoTitle, kCmdEditUndo);
    SetItemIcon(kCmdEditUndo, EditorIcons::kUndo);
    AddItem(editMenu, redoTitle, kCmdEditRedo);
}

// Window: every editor panel, then the Layout submenu.
uint32_t EditorToolbar::FillWindowMenu(uint32_t windowMenu)
{
    if (m_PanelManager)
    {
        const auto panelEntries = m_PanelManager->GetPanelMenuEntries();
        const std::size_t panelLimit = static_cast<std::size_t>(
            kWindowPanelCommandLast - kWindowPanelCommandFirst + 1);
        for (std::size_t i = 0; i < std::min(panelEntries.size(), panelLimit); ++i)
        {
            const auto& entry = panelEntries[i];
            const uint32_t commandId = kWindowPanelCommandFirst + static_cast<uint32_t>(i);
            AddItem(windowMenu, entry.title, commandId);
            SetItemIcon(commandId, entry.iconPath);
            m_WindowPanelCommands.emplace(commandId, entry.panelId);
        }
    }
    const uint32_t layoutMenu = AddSubMenu(windowMenu, "Layout");
    AddItem(layoutMenu, "Reset Layout", kCmdWindowLayoutReset);
    SetItemIcon(kCmdWindowLayoutReset, EditorIcons::kReset);
    return layoutMenu;
}

// The native module items (EditorMenuRegistry) come before the script items within a menu. File/Exit
// becomes the application's quit command where the platform has one.
std::vector<EditorMenuPathItem> EditorToolbar::CollectContributedMenuItems()
{
    const auto nativeItems = Editor::EditorMenuRegistry::Get().Snapshot(); // pre-sorted
    auto scriptItems = Editor::ScriptMenuRegistry::Get().GetToolbarItems();
    std::sort(scriptItems.begin(), scriptItems.end(),
              [](const Editor::ScriptToolbarMenuItem& a, const Editor::ScriptToolbarMenuItem& b)
              {
                  if (a.priority != b.priority)
                      return a.priority < b.priority;
                  return a.path < b.path;
              });

    std::vector<EditorMenuPathItem> items;
    items.reserve(nativeItems.size() + scriptItems.size());
    for (const auto& item : nativeItems)
    {
        if (item.Path == "File/Exit" && m_Toolbar->SetApplicationQuitCommand(item.CommandId))
            continue;
        std::string shortcut = item.ShortcutGroup.empty() || item.ShortcutName.empty()
                                   ? std::string()
                                   : Editor::CatalogShortcutLabel(item.ShortcutGroup.c_str(), item.ShortcutName.c_str());
        items.push_back({item.Path, item.CommandId, std::move(shortcut), item.SeparatorBefore});
    }
    for (const auto& item : scriptItems)
    {
        if (!item.path.empty() && item.commandId != 0)
            items.push_back({item.path, item.commandId, {}});
    }
    return items;
}

} // namespace GameEngine
