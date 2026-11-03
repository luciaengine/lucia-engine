#pragma once

#include "ECS/Systems.h"

namespace GameEngine::Engine::Renderer
{
class RenderServices;
} // namespace GameEngine::Engine::Renderer

namespace GameEngine
{
namespace TileMapECS
{

/**
 * @brief Gathers the frame's TileMapLayerRef entities into TileMapRenderFeature
 * and drains the runtime store's hot-reload queue.
 *
 * The renderer has no ECS access of its own, so this is the only seam between
 * the world and the tilemap GPU state.
 */
class TileMapExtractionSystem : public ECS::ISystem
{
public:
    explicit TileMapExtractionSystem(Engine::Renderer::RenderServices* renderServices);

    const char* GetName() const override { return "TileMapExtractionSystem"; }
    void Update(ECS::World& world, float32 deltaTime) override;

private:
    Engine::Renderer::RenderServices* m_RenderServices = nullptr;
};

} // namespace TileMapECS
} // namespace GameEngine
