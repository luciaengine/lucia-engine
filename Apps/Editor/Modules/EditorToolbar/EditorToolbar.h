#pragma once

#include "Events/Event.h"
#include "Platform/Toolbar.h" // INativeToolbar
#include "Types/ScopedSubscription.h"
#include "UI/UIElement.h"
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace GameEngine
{

class EditorApplication; // fwd
class EditorPanelManager; // fwd
class EditorTitleBar;
struct EditorMenuPathItem;
namespace Platform
{
class Window;
}                     // namespace Platform
class INativeToolbar; // fwd (Platform/Toolbar)

// EditorToolbar (Editor-level): manages menu structure and actions; uses platform-native toolbar under the hood.
class EditorToolbar
{
  public:
    EditorToolbar() = default;
    ~EditorToolbar();

    // Install the Editor toolbar on the given Platform::Window. Returns true on success.
    bool Install(Platform::Window* window, EditorApplication* app);
    void Uninstall();

    // Rebuild the menu structure (used to refresh Undo/Redo labels).
    void Refresh();
    /// Per-frame update: rebuilds menus after registry changes, binds the title bar
    /// under root and synchronizes the caption state.
    void Update(UIElement* root);
    void SetPanelManager(EditorPanelManager* panelManager) { m_PanelManager = panelManager; }
    /// The text of the title bar's caption row: the scene and the project.
    void SetCaption(std::string caption) { m_Caption = std::move(caption); }

    // Efficiently refresh just the Undo/Redo menu item titles without
    // tearing down and rebuilding the entire toolbar.
    void UpdateUndoRedoTitles();

    // Editor-level wrappers to add menus/items without exposing platform types
    void Clear();
    uint32_t AddMenu(const std::string& title);
    uint32_t AddSubMenu(uint32_t parentMenuId, const std::string& title);
    void AddItem(uint32_t parentMenuId, const std::string& title, uint32_t commandId);
    void SetItemIcon(uint32_t commandId, const std::string& imagePath);

  private:
    void AttachToUI(UIElement* root);
    void Build(EditorApplication* app);
    void FillEditMenu(EditorApplication* app, uint32_t editMenu);
    uint32_t FillWindowMenu(uint32_t windowMenu);
    std::vector<EditorMenuPathItem> CollectContributedMenuItems();

    std::unique_ptr<INativeToolbar> m_Toolbar; // platform-native toolbar instance
    EditorApplication* m_App = nullptr;        // non-owning, cleared on Uninstall
    EditorPanelManager* m_PanelManager = nullptr; // non-owning, owned by EditorApplication
    Platform::Window* m_Window = nullptr;
    UIElement::WeakRef<EditorTitleBar> m_TitleBar;
    ScopedSubscription m_ExitItem;
    // Rebuilds the menus so their shortcut text follows a changed binding.
    EventSubscription m_ShortcutBindingsSaved;
    std::string m_Caption;
    bool m_RefreshPending = false;
    std::unordered_map<uint32_t, std::string> m_WindowPanelCommands;
};

} // namespace GameEngine
