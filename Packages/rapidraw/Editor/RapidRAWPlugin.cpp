#if defined(__APPLE__)
#include "Editor/MediaWorkspaces/MediaWorkspacePlugin.h"
#include "Editor/Registries/EditorPluginRegistry.h"

namespace GameEngine::Editor
{
namespace
{
struct RegisterRapidRAWPlugin
{
    RegisterRapidRAWPlugin()
    {
        EditorPluginRegistry::Get().RegisterPlugin(GetMediaWorkspaceEditorPlugin(MediaWorkspaceKind::RapidRAW));
    }
};
RegisterRapidRAWPlugin s_RegisterPlugin;
}
}
#endif
