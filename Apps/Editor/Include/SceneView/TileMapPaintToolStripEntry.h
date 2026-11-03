#pragma once

#include "SceneView/SceneViewToolStripRegistry.h"

#include <memory>

namespace GameEngine::Editor
{
class TileMapEditContext;
namespace SceneTools
{
class TileMapPaintTool;
}

inline constexpr const char* kTileMapPaintToolId = "tileMapPaint";

/** Registers a per-view paint tool that shares the palette's authoring state. */
SceneViewToolStripEntry MakeTileMapPaintToolStripEntry(std::shared_ptr<TileMapEditContext> editContext);

/** Resolves the feature-owned tool through the view's registered-tool API. */
SceneTools::TileMapPaintTool* GetTileMapPaintTool(SceneViewController& view);

} // namespace GameEngine::Editor
