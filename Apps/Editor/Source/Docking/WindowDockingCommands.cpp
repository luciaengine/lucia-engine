#include "Docking/WindowDockingController.h"

#include "Automation/UiReplayCommandIds.h"
#include "Editor/Registries/EditorMenuRegistry.h"
#include "EditorPanelIds.h"
#include "Platform/Window.h"
#include "UI/Controls/Mount.h"
#include "UI/Layout/Docking.h"
#include "UI/UIManager.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace GameEngine::Editor
{
namespace
{

struct PanelCommand
{
    std::uint32_t CommandId;
    const char* PanelId;
};

constexpr PanelCommand kUndockCommands[] = {
    {UiReplayCommandIds::UndockHierarchy, EditorPanelIds::Hierarchy},
    {UiReplayCommandIds::UndockSceneView, EditorPanelIds::SceneView},
    {UiReplayCommandIds::UndockGameView, EditorPanelIds::GameView},
    {UiReplayCommandIds::UndockInspector, EditorPanelIds::Inspector},
    {UiReplayCommandIds::UndockNodeGraph, EditorPanelIds::NodeGraph},
    {UiReplayCommandIds::UndockAssets, EditorPanelIds::Assets},
};

constexpr PanelCommand kFloatingWindowAsserts[] = {
    {UiReplayCommandIds::AssertFloatingHierarchyWindow, EditorPanelIds::Hierarchy},
    {UiReplayCommandIds::AssertFloatingSceneViewWindow, EditorPanelIds::SceneView},
    {UiReplayCommandIds::AssertFloatingGameViewWindow, EditorPanelIds::GameView},
    {UiReplayCommandIds::AssertFloatingInspectorWindow, EditorPanelIds::Inspector},
    {UiReplayCommandIds::AssertFloatingAssetsWindow, EditorPanelIds::Assets},
};

bool FailCommand(std::string* outError, std::string message)
{
    if (outError)
        *outError = std::move(message);
    return false;
}

bool SubtreeHasClass(UIElement* root, const char* className)
{
    std::vector<UIElement*> stack{root};
    while (!stack.empty())
    {
        UIElement* current = stack.back();
        stack.pop_back();
        if (!current)
            continue;
        if (current->HasClass(className))
            return true;
        current->ForEachLogicalChild([&stack](UIElement* child) { stack.push_back(child); });
    }
    return false;
}

// Whether a mounted panel shows its real content rather than an empty shell.
// Panels without a known content marker pass on the mount alone.
bool ShowsPanelContent(UIElement& panelRoot, const char* panelId)
{
    if (std::strcmp(panelId, EditorPanelIds::Hierarchy) == 0)
        return SubtreeHasClass(&panelRoot, "hierarchy-panel") && SubtreeHasClass(&panelRoot, "tree");
    if (std::strcmp(panelId, EditorPanelIds::Inspector) == 0)
        return SubtreeHasClass(&panelRoot, "inspector-panel") && SubtreeHasClass(&panelRoot, "inspector-scrollview");
    if (std::strcmp(panelId, EditorPanelIds::Assets) == 0)
        return panelRoot.FindById("assets-tree") && panelRoot.FindById("assets-views-container");
    return true;
}

// A floating window hosts exactly `panelId` as its only dock leaf, mounted in
// that window's own UI and showing its content.
bool FloatingWindowHostsPanel(const EditorApplication::EditorWindowContext& win, const char* panelId)
{
    if (!win.window || win.floatingPanelId != panelId || !win.docking || !win.ui)
        return false;
    const DockNode* root = win.docking->GetRoot();
    if (!root || !root->IsLeaf())
        return false;
    const auto& tabs = root->GetTabs();
    if (std::find(tabs.begin(), tabs.end(), panelId) == tabs.end())
        return false;

    UIElement* rootElement = win.ui->GetRootElement();
    if (!rootElement)
        return false;
    auto* mount = dynamic_cast<Mount*>(rootElement->FindById(std::string("mount:") + panelId));
    UIElement* panelRoot = mount ? mount->GetTarget() : nullptr;
    if (!panelRoot || panelRoot->GetOwnerManager() != win.ui.get())
        return false;
    return ShowsPanelContent(*panelRoot, panelId);
}

} // namespace

void WindowDockingController::RegisterCommands()
{
    auto& registry = EditorMenuRegistry::Get();
    for (const PanelCommand& command : kUndockCommands)
    {
        registry.RegisterCommand({command.CommandId, [this, panelId = command.PanelId](std::string* outError) {
                                      return UndockPanelCommand(panelId, outError);
                                  }});
    }
    for (const PanelCommand& command : kFloatingWindowAsserts)
    {
        registry.RegisterCommand({command.CommandId, [this, panelId = command.PanelId](std::string* outError) {
                                      return ExpectFloatingWindowCommand(panelId, outError);
                                  }});
    }
    registry.RegisterCommand({UiReplayCommandIds::CloseLastFloatingWindow, [this](std::string* outError) {
                                  return CloseLastFloatingWindowCommand(outError);
                              }});
    registry.RegisterCommand({UiReplayCommandIds::ReattachLastFloatingWindow, [this](std::string* outError) {
                                  return ReattachLastFloatingWindowCommand(outError);
                              }});
}

bool WindowDockingController::UndockPanelCommand(const char* panelId, std::string* outError)
{
    if (!m_Deps.GetMainDocking || !m_Deps.GetMainDocking() || !m_Deps.QueueUndockPanel)
        return FailCommand(outError, "docking unavailable");
    m_Deps.QueueUndockPanel(panelId);
    return true;
}

bool WindowDockingController::CloseLastFloatingWindowCommand(std::string* outError)
{
    if (!m_Deps.Windows || m_Deps.Windows->size() <= 1)
        return FailCommand(outError, "no floating windows");
    const auto& windows = *m_Deps.Windows;
    for (size_t i = windows.size(); i-- > 1;)
    {
        EditorWindowContext* win = windows[i].get();
        if (win && win->window)
        {
            win->window->RequestClose();
            return true;
        }
    }
    return FailCommand(outError, "no closeable floating window");
}

bool WindowDockingController::ReattachLastFloatingWindowCommand(std::string* outError)
{
    if (!m_Deps.Windows || m_Deps.Windows->size() <= 1)
        return FailCommand(outError, "no floating windows");
    const auto& windows = *m_Deps.Windows;
    EditorWindowContext* source = nullptr;
    for (size_t i = windows.size(); i-- > 1;)
    {
        EditorWindowContext* win = windows[i].get();
        if (win && win->window && !win->floatingPanelId.empty())
        {
            source = win;
            break;
        }
    }
    EditorWindowContext* target = windows[0].get();
    if (!source || !target)
        return FailCommand(outError, "source/target window unavailable");

    DropCandidate candidate{};
    candidate.source = source;
    candidate.target = target;
    candidate.kind = DropKind::None;
    candidate.edge = DockPosition::Center;
    PerformDockDrop(candidate);
    return true;
}

bool WindowDockingController::ExpectFloatingWindowCommand(const char* panelId, std::string* outError) const
{
    if (m_Deps.Windows)
    {
        const auto& windows = *m_Deps.Windows;
        for (size_t i = 1; i < windows.size(); ++i)
        {
            if (windows[i] && FloatingWindowHostsPanel(*windows[i], panelId))
                return true;
        }
    }
    return FailCommand(outError, std::string("expected floating window hosting panel '") + panelId +
                                     "' with mounted content");
}

} // namespace GameEngine::Editor
