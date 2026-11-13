#include "Editor/Application/MainWindowTitle.h"

namespace GameEngine::Editor
{
MainWindowTitle MainWindowTitle::Format(std::string_view scene, bool sceneDirty, std::string_view project,
                                        std::string_view session)
{
    MainWindowTitle title;
    title.Caption = scene;
    if (!scene.empty() && sceneDirty)
        title.Caption += '*';
    if (!scene.empty() && !project.empty())
        title.Caption += " - ";
    title.Caption += project;

    title.WindowTitle = title.Caption;
    if (!session.empty())
    {
        if (!title.WindowTitle.empty())
            title.WindowTitle += "  ";
        title.WindowTitle += '[';
        title.WindowTitle += session;
        title.WindowTitle += ']';
    }
    return title;
}
} // namespace GameEngine::Editor
