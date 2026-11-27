#include "Editor/MediaWorkspaces/MediaWorkspacePlugin.h"
#include "Editor/Registries/EditorPluginRegistry.h"

namespace GameEngine::Editor
{
namespace
{
struct RegisterEditroomPlugin
{
    RegisterEditroomPlugin()
    {
        EditorPluginRegistry::Get().RegisterPlugin(GetMediaWorkspaceEditorPlugin(MediaWorkspaceKind::Editroom));
    }
};
RegisterEditroomPlugin s_RegisterPlugin;
}
}
