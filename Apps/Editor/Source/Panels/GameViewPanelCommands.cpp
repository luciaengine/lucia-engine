#include "Panels/GameViewPanel.h"

#include "Automation/UiReplayCommandIds.h"
#include "Editor/Registries/EditorMenuRegistry.h"

#include <string>

namespace GameEngine
{

void GameViewPanel::RegisterCommands()
{
    Editor::EditorMenuRegistry::Get().RegisterCommand(
        {UiReplayCommandIds::AssertGameViewNoCameraOverlayVisible, [this](std::string* outError) {
             if (IsNoCameraOverlayVisible())
                 return true;
             if (outError)
                 *outError = "game view no-camera overlay is not visible";
             return false;
         }});
}

} // namespace GameEngine
