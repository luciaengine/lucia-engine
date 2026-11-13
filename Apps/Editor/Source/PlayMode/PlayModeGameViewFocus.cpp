#include "PlayMode/PlayModeGameViewFocus.h"

#include "Core/DeferredActionQueue.h"
#include "EditorPanelIds.h"
#include "PlayMode/PlayModeManager.h"
#include "UI/Layout/Docking.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace GameEngine::Editor
{

namespace
{

constexpr const char* kFinishRestoreKey = "editor.play-mode-game-view-focus-restore";

bool ElementExists(const UIManager& ui, const std::string& id)
{
    UIElement* root = ui.GetRootElement();
    return root && root->FindById(HashStringId(id)) != nullptr;
}

const DockNode* FindGameViewGroup(const DockingManager& docking)
{
    std::string path;
    return docking.FindLeafContaining(EditorPanelIds::GameView, path);
}

} // namespace

PlayModeGameViewFocus::PlayModeGameViewFocus(ForEachWindow forEachWindow, DeferredActionQueue& afterUiUpdate)
    : m_ForEachWindow(std::move(forEachWindow)), m_AfterUiUpdate(afterUiUpdate)
{
}

void PlayModeGameViewFocus::OnPlayModeStateChanged(PlayModeState state)
{
    if (state == PlayModeState::Play && !m_InPlay)
    {
        m_InPlay = true;
        TakeFocus();
    }
    else if ((state == PlayModeState::Edit || state == PlayModeState::ChangeReview) && m_InPlay)
    {
        m_InPlay = false;
        RestoreFocus();
    }
}

void PlayModeGameViewFocus::TakeFocus()
{
    m_FocusedUi = nullptr;
    m_FocusBeforePlay.clear();
    m_FrontTabBeforePlay.clear();
    if (m_ForEachWindow)
        m_ForEachWindow([this](UIManager& ui, DockingManager& docking, const ActivateOpenPanel& activateOpenPanel)
                        { TakeFocusInWindow(ui, docking, activateOpenPanel); });
}

void PlayModeGameViewFocus::RestoreFocus()
{
    if (!m_FocusedUi || !m_ForEachWindow)
        return;
    m_ViewportHadFocusAtStop = false;
    m_ForEachWindow(
        [this](UIManager& ui, DockingManager&, const ActivateOpenPanel&)
        {
            if (&ui == m_FocusedUi)
                m_ViewportHadFocusAtStop = ui.GetFocusedElementId() == EditorPanelIds::GameViewViewport;
        });
    (void)m_AfterUiUpdate.EnqueueUnique(kFinishRestoreKey, [this]() { FinishRestore(); });
}

void PlayModeGameViewFocus::FinishRestore()
{
    // Play that starts again before the UI update keeps the state TakeFocus wrote.
    if (m_InPlay)
        return;
    if (m_FocusedUi && m_ForEachWindow)
        m_ForEachWindow([this](UIManager& ui, DockingManager& docking, const ActivateOpenPanel& activateOpenPanel)
                        { FinishRestoreInWindow(ui, docking, activateOpenPanel); });
    m_FocusedUi = nullptr;
    m_FocusBeforePlay.clear();
    m_FrontTabBeforePlay.clear();
}

void PlayModeGameViewFocus::TakeFocusInWindow(UIManager& ui, DockingManager& docking,
                                              const ActivateOpenPanel& activateOpenPanel)
{
    if (m_FocusedUi)
        return;
    std::string frontTab;
    if (const DockNode* group = FindGameViewGroup(docking))
        frontTab = group->GetActivePanelId();
    if (frontTab != EditorPanelIds::GameView && !activateOpenPanel(EditorPanelIds::GameView))
        return;
    if (frontTab != EditorPanelIds::GameView)
        m_FrontTabBeforePlay = std::move(frontTab);

    const std::string& previous = ui.GetFocusedElementId();
    if (previous != EditorPanelIds::GameViewViewport)
        m_FocusBeforePlay = previous;
    // An id that is not mounted yet is kept: a tab that activates by a full dock rebuild mounts
    // the viewport on the next update.
    ui.SetFocusById(EditorPanelIds::GameViewViewport);
    m_FocusedUi = &ui;
}

void PlayModeGameViewFocus::FinishRestoreInWindow(UIManager& ui, DockingManager& docking,
                                                  const ActivateOpenPanel& activateOpenPanel)
{
    if (&ui != m_FocusedUi)
        return;
    RestoreFrontTab(docking, activateOpenPanel);
    if (!m_ViewportHadFocusAtStop)
        return;
    if (!m_FocusBeforePlay.empty() && ElementExists(ui, m_FocusBeforePlay))
    {
        ui.SetFocusById(m_FocusBeforePlay);
        return;
    }
    // The Scene View takes keys with no element focused (SceneViewPanel::FocusViewport).
    ui.SetFocusById("");
}

void PlayModeGameViewFocus::RestoreFrontTab(const DockingManager& docking,
                                            const ActivateOpenPanel& activateOpenPanel) const
{
    // A tab the user picked during Play stays in front.
    if (m_FrontTabBeforePlay.empty() || !docking.IsPanelActiveTab(EditorPanelIds::GameView))
        return;
    const DockNode* group = FindGameViewGroup(docking);
    if (!group)
        return;
    const std::vector<std::string>& tabs = group->GetTabs();
    if (std::find(tabs.begin(), tabs.end(), m_FrontTabBeforePlay) == tabs.end())
        return;
    (void)activateOpenPanel(m_FrontTabBeforePlay);
}

} // namespace GameEngine::Editor
