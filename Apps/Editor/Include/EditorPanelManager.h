#pragma once

#include "EditorApplication.h" // EditorApplication::EditorWindowContext
#include "Editor/Registries/EditorPanelRegistry.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace GameEngine
{

class DockNode;
class DockingManager;
class RenderDocCapture;
class UIElement;
enum class SettingsCategory : uint64_t;
namespace Editor { class FloatingFrameHost; }
namespace Rendering { class IDevice; }
namespace Rendering::RenderGraph { class RGFrame; }

/**
 * Manages editor panel lifecycle: showing, activating, and closing dock panels.
 * Extracted from EditorApplication to keep the application class focused on
 * initialization, rendering, and input dispatch.
 */
class EditorPanelManager
{
  public:
    using EditorWindowContext = EditorApplication::EditorWindowContext;

    EditorPanelManager() = default;

    /// Set references to state owned by EditorApplication.
    void SetDocking(DockingManager* docking) { m_Docking = docking; }
    void SetPanelStorage(std::vector<std::unique_ptr<UIElement>>* storage) { m_PanelStorage = storage; }
    void SetWindows(std::vector<std::unique_ptr<EditorWindowContext>>* windows) { m_Windows = windows; }
    void SetIsLayoutDirtyCallback(std::function<bool()> callback) { m_IsLayoutDirty = std::move(callback); }
    /// The docking layer's in-canvas frames. ShowPanel asks it first: a panel
    /// that is currently a frame is raised, not docked a second time.
    void SetFloatingFrames(Editor::FloatingFrameHost* frames) { m_FloatingFrames = frames; }

    struct PanelMenuEntry
    {
        std::string panelId;
        std::string title;
        std::string iconClass;
        std::string iconPath;
    };

    /// Returns the live, sorted panel inventory used by editor panel launchers.
    std::vector<PanelMenuEntry> GetPanelMenuEntries() const;

    // -- Panel activation --

    /// Shows or activates the panel `panelId` in the given window context. A
    /// panel absent from the layout docks as a tab in the leaf holding
    /// preferredLeafPanelId when that panel is docked, otherwise at its panel
    /// type's default placement (EditorPanelTypeDescriptor::DefaultAnchorPanelId).
    void ShowOrActivatePanel(EditorWindowContext* ctx, const std::string& panelId,
                             const std::string& preferredLeafPanelId = {});

    /// Activates the Settings panel and shows `category`.
    void ShowOrActivateSettingsPanelToCategory(EditorWindowContext* ctx, SettingsCategory category);
    /// Activates the Settings panel and shows the EditorSettingsRegistry category `categoryId`.
    void ShowOrActivateSettingsPanelToRegistryCategory(EditorWindowContext* ctx, std::string_view categoryId);
    /// Activates the graph panel the user opened last, or the Shader Graph tab.
    void ShowOrActivateGraphPanel(EditorWindowContext* ctx);
    /// Opens the Web panel at the editor documentation URL in the main window.
    void ShowOrActivateHelpPanel();

    // -- Per-frame panel updates --
    // Each panel registers its callback once, so UpdatePanels dispatches through a
    // flat list rather than a type scan on every frame.
    using UpdateCallback = std::function<void(const Editor::PanelUpdateContext&)>;
    /// Wires a panel built from a registered panel type (EditorPanelRegistry):
    /// registers the type's Update hook for it, and ShowOrActivatePanel places
    /// and shows `panelId` the way the type declares.
    void AdoptPanelOfType(const std::string& panelId, UIElement* panel, std::string_view typeKey);
    // alwaysUpdate=false (default): UpdatePanels skips this entry when the
    // panel root has zero layout dimensions (inactive dock tab, hidden via
    // display:none, etc.). Set true for callbacks that must run even when
    // the panel UI is hidden — typically background data collection
    // (metric sampling, log capture queueing) that the user expects to
    // accumulate continuously and only render when the panel is reopened.
    void RegisterUpdateCallback(UIElement* panel, UpdateCallback callback, bool alwaysUpdate = false);
    void UnregisterUpdateCallback(UIElement* panel);
    void UpdatePanels(Rendering::IDevice* device,
                      RenderDocCapture* renderDoc,
                      Rendering::RenderGraph::RGFrame* rg2Frame);

    // -- Panel closing --
    void CloseActiveTabOrWindow(EditorWindowContext* ctx);

    // -- Dock tree helpers --
    static DockNode* FindFirstLeaf(DockNode* n);
    static DockNode* FindLeafContainingPanel(DockNode* n, const std::string& panelId);

    // -- Utility --
    template <typename T>
    T* FindFirstPanelOfType() const;

  private:
    friend class EditorApplication;
    void ClearUpdateCallbacks();

    /// Rebuild the "dock" DockspaceElement inside ctx after a model change.
    static void RebuildDockspace(EditorWindowContext* ctx);
    /// Rebuild immediately when a panel must be detached or mounted within the current event.
    static void RebuildDockspaceNow(EditorWindowContext* ctx, DockingManager* model);
    static void RefreshPanelUI(EditorWindowContext* ctx, const std::string& panelId);

    /// The panel type `panelId` was adopted with; an empty descriptor for panels
    /// no type built (package panels, per-asset graph docks).
    Editor::EditorPanelTypeDescriptor PanelTypeOf(const std::string& panelId) const;
    /// Queues the type's OnShown hook for the panel at the next UI safe point.
    void QueueShownHook(EditorWindowContext* ctx, const std::string& panelId,
                        const Editor::EditorPanelTypeDescriptor& type);
    bool IsStoredPanel(const UIElement* panel) const;

    struct UpdateEntry
    {
        UIElement* panel = nullptr;
        UpdateCallback callback;
        bool alwaysUpdate = false;
    };

    DockingManager* m_Docking = nullptr;
    std::vector<std::unique_ptr<UIElement>>* m_PanelStorage = nullptr;
    std::vector<std::unique_ptr<EditorWindowContext>>* m_Windows = nullptr;
    std::vector<UpdateEntry> m_UpdateCallbacks;
    std::vector<UpdateEntry> m_PendingCallbacks;
    std::unordered_map<std::string, std::string> m_PanelTypeKeys; // panel id -> panel type key
    bool m_UpdatingPanels = false;
    std::function<bool()> m_IsLayoutDirty;
    Editor::FloatingFrameHost* m_FloatingFrames = nullptr;
};

// Template implementation
template <typename T>
T* EditorPanelManager::FindFirstPanelOfType() const
{
    if (!m_PanelStorage)
        return nullptr;
    for (const auto& p : *m_PanelStorage)
    {
        if (!p)
            continue;
        if (auto* casted = dynamic_cast<T*>(p.get()))
            return casted;
    }
    return nullptr;
}

} // namespace GameEngine
