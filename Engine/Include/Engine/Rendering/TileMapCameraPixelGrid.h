#pragma once

#include <cstdint>

namespace GameEngine
{
namespace ECS
{
class World;
} // namespace ECS
namespace Components
{
struct Camera;
} // namespace Components
} // namespace GameEngine

namespace GameEngine::Engine::Renderer
{

// Texel density of the tilemap content in `world` (source texels per world unit,
// e.g. 16 for a 16 px tile on a one-unit cell), or 0 when the scene has no
// resolvable tilemap layer.
//
// The lowest visible (map GUID, layer id) supplies the density when its assets resolve. A scene mixing
// densities cannot be pixel-perfect for all of them at once, so this picks one
// rather than averaging into a density that is correct for nothing.
float ResolveScenePixelsPerUnit(ECS::World& world);

// Pixels-per-unit the pixel-perfect camera should actually render at, and the ONE
// derivation shared by the editor Scene View and the game / Player camera so both
// land on the same pixel grid.
//
// In auto mode the density comes from the scene's tilemaps, so the camera tracks
// the art instead of a hand-typed number that goes stale when the tileset changes;
// the authored PixelPerfectPixelsPerUnit is the fallback whenever the scene
// resolves none, which leaves a tilemap-less scene rendering exactly as before.
// Manual mode returns the authored value untouched. Recomputed each frame, so it
// follows a tileset swap without re-authoring the camera.
//
// Always >= 1: a zero density has no inverse, and world-units-per-texel is the
// first thing every caller derives from it.
uint32_t ResolvePixelPerfectPixelsPerUnit(ECS::World& world, const Components::Camera& camera);

} // namespace GameEngine::Engine::Renderer
