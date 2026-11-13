#include "SceneView/BuiltInSceneViewTools.h"

#include "SceneView/SceneViewToolStripRegistry.h"
#include "SceneView/SplineTool/SplineToolStripEntry.h"
#include "Terrain/TerrainBrushToolStripEntry.h"

namespace GameEngine::Editor
{

void RegisterBuiltInSceneViewTools()
{
    SceneViewToolStripRegistry& registry = SceneViewToolStripRegistry::Get();
    registry.Register(MakeSplineToolStripEntry());
    registry.Register(MakeTerrainBrushToolStripEntry());
}

} // namespace GameEngine::Editor
