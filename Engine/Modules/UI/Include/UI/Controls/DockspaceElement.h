#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "UI/UIElement.h"
#include "UI/UIManagerRef.h"
#include "UI/Layout/Docking.h"
#include "UI/Layout/DockingHitTest.h"

namespace GameEngine {

class Mount;
class DockLeaf;
class Label;

enum class DockPatchFallbackReason : uint8_t
{
    None = 0,
    SourceLeafCollapsed,
    TargetPathInvalidated,
    FloatingTabbarTransition,
    MissingUINodes,
    TabVectorMismatch,
    EventDispatchActive
};

class DockspaceElement final : public UIElement {
public:
    DockspaceElement() = default;
    ~DockspaceElement() override;

    void BindModel(DockingManager* model) { m_Model = model; MarkDirty(VisualDirty | ChildrenDirty | LayoutDirty); }

    DockingManager* GetModel() const { return m_Model; }

    // Queues a rebuild with the owning UIManager, which runs it after event dispatch.
    // The dirty mark wakes the idle and pointer-only gates: some callers change only
    // the DockingManager model and mark no element dirty.
    void RequestRebuildFromModel();

    // Hands a pending rebuild request to the new owner.
    void OnOwnerManagerChanged(UIManager* owner) override;

    bool RequestActivationPatch(const std::string& panelId);
    bool RequestTabReorder(const std::string& leafPath);
    bool RequestTabRemoval(const std::string& panelId);
    bool RequestTabAddition(const std::string& leafPath, const std::string& panelId);

    void RebuildFromModel();

    /** Invoked at the end of RebuildFromModel (e.g. editor can re-attach per-tab chrome). */
    void SetOnPostRebuild(std::function<void()> cb) { m_OnPostRebuild = std::move(cb); }

    /** Invoked when user right-clicks a dock tab (panelId, screenX, screenY). */
    void SetOnTabContextMenu(std::function<void(const std::string&, float, float)> cb) { m_OnTabContextMenu = std::move(cb); }
    bool FireTabContextMenu(const std::string& panelId, float x, float y)
    {
        if (!m_OnTabContextMenu)
            return false;
        m_OnTabContextMenu(panelId, x, y);
        return true;
    }

    /** Invoked when the user closes a dock tab with a close gesture (panelId). The host closes the panel. */
    void SetOnTabCloseRequested(std::function<void(const std::string&)> cb) { m_OnTabCloseRequested = std::move(cb); }
    /** Closes a tab's panel through the host's close handler, or removes the tab when no handler is set. */
    void RequestTabClose(const std::string& panelId);

    struct DockInstrumentation
    {
        uint32_t fullRebuilds = 0;
        uint32_t activationPatches = 0;
        uint32_t reorderPatches = 0;
        uint32_t tabRemovalPatches = 0;
        uint32_t tabAdditionPatches = 0;
        uint32_t patchFallbacks = 0;
        DockPatchFallbackReason lastFallbackReason = DockPatchFallbackReason::None;
    };
    const DockInstrumentation& GetInstrumentation() const { return m_Instrumentation; }

    void SetDropPreview(const DockDropTarget& target, float containerW, float containerH);
    void ClearDropPreview();
    void ResetLayoutDefault();
    void SyncSplitRatiosFromUI();

    bool ToggleMaximizeLeaf(const std::string& panelId);
    bool IsMaximized() const { return m_SavedRoot != nullptr; }

    void ShowDockDragGhost(const std::string& title, float mouseX, float mouseY, float tabW = 0.f, float tabH = 0.f);
    void UpdateDockDragGhostPosition(float mouseX, float mouseY);
    void SetDockDragGhostUndockMode(bool undocking);
    void HideDockDragGhost();

    // Accent color forwarded to dock drop overlays (ARGB: 0xAARRGGBB)
    void SetAccentColor(uint32_t argb) { m_AccentColor = argb; }

private:
    // UIManager drains the request when it runs the queued rebuild.
    friend class UIManager;
    bool ConsumeRebuildRequest() { bool v = m_NeedsRebuild; m_NeedsRebuild = false; return v; }

    // Shared leaf-view resolver: finds UI elements for a leaf given a panelId.
    struct LeafView
    {
        DockLeaf* leaf = nullptr;
        UIElement* tabBar = nullptr;
        UIElement* content = nullptr;
        Mount* mount = nullptr;
        std::string leafPath;
    };
    LeafView ResolveLeafByPanelId(const std::string& panelId);
    LeafView ResolveLeafByPath(const std::string& leafPath);

    void ApplyActiveTabVisuals(const LeafView& view, const std::string& activePanelId);

    void BuildFromNode(const DockNode* n, UIElement* parent, const std::string& path);
    void SyncSplitRatiosRecursive(UIElement* el, const std::string& path);
    bool PatchActiveTabInLeaf(const std::string& panelId);

    void Fallback(DockPatchFallbackReason reason);
    void QueueRebuildWith(UIManager* owner);
    void CancelQueuedRebuild();

    DockingManager* m_Model = nullptr;
    bool m_NeedsRebuild = false;
    UIManagerRef m_RebuildQueuedWith;
    DockInstrumentation m_Instrumentation{};
    std::function<void()> m_OnPostRebuild;
    std::function<void(const std::string&, float, float)> m_OnTabContextMenu;
    std::function<void(const std::string&)> m_OnTabCloseRequested;

    std::unique_ptr<DockNode> m_SavedRoot;
    uint32_t m_AccentColor = 0xFF3A8FFF;

    UIElement* m_DragGhost = nullptr; // Non-owning; child of root element
    Label* m_DragGhostLabel = nullptr;
    Label* m_DragGhostUndockLabel = nullptr;
    UIElement* m_DragGhostTab = nullptr;
    UIElement* m_DragGhostBody = nullptr;
};

} // namespace GameEngine
