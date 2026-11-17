#pragma once

#include <string>
#include <string_view>

namespace GameEngine::Editor
{
/// The caption row and the operating system's title (taskbar, window switcher). The caption reads
/// "<scene> - <project>", with "*" after a scene that has unsaved changes. The operating system's
/// title starts with "Lucia Engine - " before the caption, and adds the session in brackets when the
/// editor runs from a worktree or with a session label.
struct MainWindowTitle
{
    std::string Caption;
    std::string WindowTitle;

    /// `scene`, `project` and `session` may be empty; `sceneDirty` marks a scene with unsaved changes.
    static MainWindowTitle Format(std::string_view scene, bool sceneDirty, std::string_view project,
                                  std::string_view session);
};
} // namespace GameEngine::Editor
