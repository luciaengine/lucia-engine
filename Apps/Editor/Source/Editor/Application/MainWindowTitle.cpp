#include "Editor/Application/MainWindowTitle.h"

namespace GameEngine::Editor
{
namespace
{
constexpr std::string_view kApplicationName = "Lucia Engine";
}

MainWindowTitle MainWindowTitle::Format(std::string_view scene, bool sceneDirty, std::string_view project,
                                        std::string_view session)
{
    MainWindowTitle title;
    title.Caption = kApplicationName;
    if (!project.empty())
    {
        title.Caption += " - ";
        title.Caption += project;
    }
    if (!scene.empty())
    {
        title.Caption += " - ";
        title.Caption += scene;
        if (sceneDirty)
            title.Caption += '*';
    }

    title.WindowTitle = title.Caption;
    if (!session.empty())
    {
        title.WindowTitle += "  ";
        title.WindowTitle += '[';
        title.WindowTitle += session;
        title.WindowTitle += ']';
    }
    return title;
}
} // namespace GameEngine::Editor
