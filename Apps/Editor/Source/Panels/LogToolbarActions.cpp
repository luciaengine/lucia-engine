#include "Panels/LogToolbarActions.h"

#include "Editor/EditorPaths.h"
#include "Logger/Logger.h"
#include "UI/Controls/Button.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"

#include <string>
#include <utility>

namespace GameEngine
{
namespace
{
constexpr const char* kLayoutPath = "UI/controls/LogToolbarActions.uxml";
}

LogToolbarActions::LogToolbarActions(std::function<void()> clear, std::function<void()> copy,
                                   std::function<bool()> togglePaused,
                                   std::function<void(UIElement&)> toggleChannels)
    : m_Clear(std::move(clear)), m_Copy(std::move(copy)), m_TogglePaused(std::move(togglePaused)),
      m_ToggleChannels(std::move(toggleChannels))
{
    AddClass("log-toolbar-main-actions");
    RequestSubtreeStyleAssetPath("UI/panels/LogPanel.css", "editor");
}

void LogToolbarActions::OnOwnerManagerChanged(UIManager* owner)
{
    if (!owner || m_BindAttempted || m_BindScheduled)
        return;
    m_BindScheduled = true;
    PostSafeAction([this]() { BindLayout(); });
}

void LogToolbarActions::BindLayout()
{
    m_BindScheduled = false;
    UIManager* manager = GetOwnerManager();
    if (!manager || m_BindAttempted)
        return;
    m_BindAttempted = true;
    const std::string layoutPath = (Editor::GetEditorGlobalPaths().installAssetsRoot / kLayoutPath).make_preferred().string();
    if (!manager->InstantiateLayoutChildrenFromAssetPath(this, std::string("editor:") + kLayoutPath))
    {
        Logger::Log::Error("LogToolbarActions: cannot load {}; check the staged editor asset mount. "
                           "The log view remains available.",
                           layoutPath);
        return;
    }
    auto* clear = dynamic_cast<Button*>(FindById("LogClear"));
    auto* copy = dynamic_cast<Button*>(FindById("LogCopy"));
    m_PauseButton = dynamic_cast<Button*>(FindById("LogPause"));
    m_PauseIcon = FindById("LogPauseIcon");
    if (!clear || !copy || !m_PauseButton || !m_PauseIcon)
    {
        Logger::Log::Error("LogToolbarActions: {} requires Clear, Pause, Copy, and the Pause icon; "
                           "check the staged layout. The log view remains available.",
                           layoutPath);
        return;
    }
    clear->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { m_Clear(); });
    copy->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { m_Copy(); });
    if (auto* channels = dynamic_cast<Button*>(FindById("LogChannels")))
    {
        channels->SetTooltip("Show or hide log channels and set the level each channel logs at.");
        channels->RegisterEventHandler(kEventButtonClick, [this](UIEvent& event) {
            m_ToggleChannels(*event.CurrentTarget);
        });
    }
    m_PauseButton->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { TogglePaused(); });
}

void LogToolbarActions::TogglePaused()
{
    const bool paused = m_TogglePaused();
    m_PauseButton->SetText(paused ? "Resume" : "Pause");
    m_PauseIcon->RemoveClass(paused ? "log-action-pause-icon" : "log-action-resume-icon");
    m_PauseIcon->AddClass(paused ? "log-action-resume-icon" : "log-action-pause-icon");
}
} // namespace GameEngine
