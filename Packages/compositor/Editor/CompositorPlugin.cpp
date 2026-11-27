#include "Editor/MediaWorkspaces/MediaWorkspacePlugin.h"
#include "Editor/Registries/EditorPluginRegistry.h"

namespace GameEngine::Editor
{
namespace
{
struct RegisterCompositorPlugin
{
    RegisterCompositorPlugin()
    {
        EditorPluginRegistry::Get().RegisterPlugin(GetMediaWorkspaceEditorPlugin(MediaWorkspaceKind::Compositor));
    }
};
RegisterCompositorPlugin s_RegisterPlugin;
}
}
