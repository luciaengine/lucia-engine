#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace GameEngine
{
class DeferredActionQueue;
class DockingManager;
class UIManager;

namespace Editor
{
enum class PlayModeState : std::uint8_t;

/// Gives the Game View keyboard focus while the game plays, so the game's keys reach the game and
/// not the editor control that had the focus. Stop gives the focus back.
class PlayModeGameViewFocus
{
  public:
    /// Brings an open panel of the visited window to the front and never opens a closed one
    /// (EditorPanelManager::ActivateOpenPanel). False when the panel is not open there.
    using ActivateOpenPanel = std::function<bool(const std::string& panelId)>;
    using WindowVisitor =
        std::function<void(UIManager& ui, DockingManager& docking, const ActivateOpenPanel& activateOpenPanel)>;
    /// Calls the visitor once for each open editor window.
    using ForEachWindow = std::function<void(const WindowVisitor&)>;

    /// Stop runs its restore from `afterUiUpdate`, which the editor flushes after the UI update, so
    /// the layout that play fullscreen replaced is mounted again first.
    PlayModeGameViewFocus(ForEachWindow forEachWindow, DeferredActionQueue& afterUiUpdate);

    /// Play: the Game View's tab comes to the front of its dock group and its viewport takes the
    /// focus. A closed Game View stays closed. Stop: the tab that was in front comes back unless
    /// the user picked another; when the viewport still has the focus, the focus goes back to the
    /// element that had it before Play, or to the Scene View.
    void OnPlayModeStateChanged(PlayModeState state);

  private:
    void TakeFocus();
    void RestoreFocus();
    void FinishRestore();
    void TakeFocusInWindow(UIManager& ui, DockingManager& docking, const ActivateOpenPanel& activateOpenPanel);
    void FinishRestoreInWindow(UIManager& ui, DockingManager& docking, const ActivateOpenPanel& activateOpenPanel);
    void RestoreFrontTab(const DockingManager& docking, const ActivateOpenPanel& activateOpenPanel) const;

    ForEachWindow m_ForEachWindow;
    DeferredActionQueue& m_AfterUiUpdate;
    // The window whose Game View took the focus at Play; null when no restore is due. Compared
    // only, never read through: the window can close before the restore.
    const UIManager* m_FocusedUi = nullptr;
    std::string m_FocusBeforePlay;
    // The tab in front of the Game View's dock group before Play brought the Game View forward.
    std::string m_FrontTabBeforePlay;
    // Read at Stop, before the layout changes: true when the viewport still had the focus.
    bool m_ViewportHadFocusAtStop = false;
    bool m_InPlay = false;
};

} // namespace Editor
} // namespace GameEngine
