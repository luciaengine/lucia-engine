#include "SceneView/TileMapPaintToolStripEntry.h"

#include "SceneView/TileMapPaintTool.h"
#include "SceneViewController.h"
#include "TileMap/TileMapEditContext.h"
#include "UI/EditorIcons.h"

#include <memory>
#include <utility>

namespace GameEngine::Editor
{
namespace
{
std::unique_ptr<SceneTools::ISceneTool> CreateTileMapPaintToolForView(
    SceneViewController& owner, const std::shared_ptr<TileMapEditContext>& editContext)
{
    auto tool = std::make_unique<SceneTools::TileMapPaintTool>();
    tool->SetEditContext(editContext);
    tool->SetUndoRedoService(owner.GetUndoRedoService());
    tool->SetRenderServices(owner.GetRenderServices());
    return tool;
}
} // namespace

SceneViewToolStripEntry MakeTileMapPaintToolStripEntry(std::shared_ptr<TileMapEditContext> editContext)
{
    SceneViewToolStripEntry entry;
    entry.Id = kTileMapPaintToolId;
    entry.Tooltip = "Tile Map Paint";
    entry.Icon = EditorIcons::kBrush;
    entry.CreateTool = [context = std::move(editContext)](SceneViewController& owner) {
        return CreateTileMapPaintToolForView(owner, context);
    };
    return entry;
}

SceneTools::TileMapPaintTool* GetTileMapPaintTool(SceneViewController& view)
{
    return dynamic_cast<SceneTools::TileMapPaintTool*>(view.GetRegisteredTool(kTileMapPaintToolId));
}

} // namespace GameEngine::Editor
