#pragma once

#include <string>
#include <string_view>

namespace GameEngine::Editor
{
/// Both titles read "Lucia Engine - <project> - <scene>", with "*" after an unsaved scene.
/// The operating system title also includes the session label when present.
struct MainWindowTitle
{
    std::string Caption;
    std::string WindowTitle;

    /// `scene`, `project` and `session` may be empty; `sceneDirty` marks a scene with unsaved changes.
    static MainWindowTitle Format(std::string_view scene, bool sceneDirty, std::string_view project,
                                  std::string_view session);
};
} // namespace GameEngine::Editor
