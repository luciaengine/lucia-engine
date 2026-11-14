#pragma once

namespace GameEngine::Engine::Renderer
{
class RenderServices;
} // namespace GameEngine::Engine::Renderer

namespace GameEngine::ECS
{
class SystemScheduleBuilder;
} // namespace GameEngine::ECS

namespace GameEngine
{
namespace TileMapECS
{

void AddTileMapSystemsToSchedule(ECS::SystemScheduleBuilder& builder,
                                 Engine::Renderer::RenderServices* renderServices);

} // namespace TileMapECS
} // namespace GameEngine
