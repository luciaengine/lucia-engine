#include "EditorPanelIds.h"

#include <cstring>

namespace GameEngine::EditorPanelIds
{

const char* DefaultPanelLeafAnchor(std::string_view panelId)
{
    if (panelId == Hierarchy || panelId == Todo ||
        panelId == Bookmarks)
        return Hierarchy;

    if (panelId == Inspector || panelId == Inspector2 ||
        panelId == UndoHistory || panelId == Log)
        return Inspector;

    /* Per-asset graph docks are "{kind tab}:{guid}" (MakeGraphDockIdForAsset).
       They belong with their kind tab in the centre workspace, not in whatever
       leaf happens to come first in the tree. */
    for (const char* graphTab : {NodeGraph, AnimationGraph,
                                 GameLogicGraph})
    {
        const std::size_t len = std::strlen(graphTab);
        if (panelId.size() > len && panelId.substr(0, len) == graphTab && panelId[len] == ':')
            return SceneView;
    }

    if (panelId == SceneView || panelId == GameView ||
        panelId == Settings || panelId == NodeGraph || panelId == AnimationGraph ||
        panelId == GameLogicGraph || panelId == Web ||
        panelId == Diff || panelId == UiDemo ||
        panelId == ScriptEditor || panelId == VisualProfiler ||
        panelId == RenderGraph || panelId == Vram)
        return SceneView;

    if (panelId == AssetView)
        return AssetView;

    // Assets, script/CPU diagnostics, animation tools, project utilities, and
    // package-contributed panels default to the bottom workspace stack.
    return panelId == Assets ? ScriptErrors : Assets;
}

} // namespace GameEngine::EditorPanelIds
