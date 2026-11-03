#include "TileMapECS/RegisterTileMap.h"

#include "ECS/SystemScheduling.h"
#include "TileMapECS/Systems/TileMapCollisionSystem.h"
#include "TileMapECS/Systems/TileMapExtractionSystem.h"


namespace GameEngine
{
namespace TileMapECS
{

void AddTileMapSystemsToSchedule(ECS::SystemScheduleBuilder& builder,
                                 Engine::Renderer::RenderServices* renderServices)
{
    // Reads WorldTransform, so it must follow TransformHierarchy; the render
    // node consumes its output later the same frame, at declaration time.
    builder.Add<TileMapExtractionSystem>("TileMapExtraction", ECS::SystemPhase::Extraction, 1,
                                         {"TransformHierarchy"}, renderServices);
    // Baked chunk bodies must exist before the step; PhysicsStep declares the
    // optional edge back to this name (PhysicsECS never links this module).
    //
    // The PhysicsInit edge is what serializes the two body-creating systems.
    // Same-wave systems run in parallel, and both grow the ContactSurfaceTable
    // index space and the backend's unlocked body/side tables — so this must be
    // a declared edge, not registration order. It subsumes the bootstrap edge:
    // PhysicsInit already depends on PhysicsWorldBootstrap.
    builder.Add<TileMapCollisionSystem>("TileMapCollision", ECS::SystemPhase::Extraction, 4,
                                        {"TransformHierarchy", "PhysicsInit"});
}

} // namespace TileMapECS
} // namespace GameEngine
