#pragma once

#include "ECS/Entity.h"
#include "Mathematics/Types.h"

#include <filesystem>

namespace GameEngine { class AssetManager; }
namespace GameEngine::ECS { class World; }
namespace GameEngine::Engine::Renderer { class RenderServices; }

namespace GameEngine::Editor
{

// Creates a "sprite" entity for a dropped texture.
//
// When `orientForCamera2D` is true (a 2D view), the entity is a Sprite2D whose pixels per unit
// is the scene's tile density (TileMapECS::ResolveScenePixelsPerUnit), or one unit per texel in a
// scene without tiles, so it matches the tiles around it.
//
// Otherwise it is a plane primitive scaled to the texture's pixel size, one unit per texel, with a
// fresh .material under <project>/Assets/Materials/ binding the texture to albedoMap, so the card
// survives scene serialization and lights with the scene.
//
// The texture must already be loaded: the factory reads it from the asset cache
// and never loads it, so it never waits on the main thread. Callers load it first
// (RunWhenAssetLoaded) and create the sprite once it lands.
//
// Returns the new entity handle; invalid on failure (bad path, null GUID, texture
// not loaded, entity allocation failure). The factory does not assign a Parent;
// callers parent the result after construction.
ECS::EntityHandle CreateSpriteEntityFromTexture(
    ECS::World& world,
    Engine::Renderer::RenderServices& rs,
    AssetManager& assets,
    const std::filesystem::path& texturePath,
    const Mathematics::Vector3& worldPos,
    bool orientForCamera2D = false);

} // namespace GameEngine::Editor
