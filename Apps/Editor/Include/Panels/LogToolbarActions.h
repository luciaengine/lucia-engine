#pragma once

#include "UI/UIElement.h"

#include <functional>

namespace GameEngine
{
class Button;
class UIManager;

/// Log action buttons, bound from their editor-mounted layout after attachment.
class LogToolbarActions : public UIElement
{
public:
    LogToolbarActions(std::function<void()> clear, std::function<void()> copy,
                      std::function<bool()> togglePaused);

private:
    void OnOwnerManagerChanged(UIManager* owner) override;
    void BindLayout();
    void TogglePaused();

    std::function<void()> m_Clear;
    std::function<void()> m_Copy;
    std::function<bool()> m_TogglePaused;
    Button* m_PauseButton = nullptr;
    UIElement* m_PauseIcon = nullptr;
    bool m_BindScheduled = false;
    bool m_BindAttempted = false;
};
} // namespace GameEngine
