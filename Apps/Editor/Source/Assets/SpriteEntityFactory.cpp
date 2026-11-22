#include "Editor/Entities/SpriteEntityFactory.h"

#include "AssetCore/Asset.h"
#include "AssetCore/AssetTypes.h"
#include "AssetCore/GUID.h"
#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Assets/TextureAsset.h"
#include "Components/Name.h"
#include "Components/Rendering/Sprite2D.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Transform.h"
#include "ECS/World.h"
#include "Editor/Entities/EntityMaterialTextureAssign.h"
#include "Editor/Entities/SpritePlacementSize.h"
#include "Engine/Rendering/PrimitiveGenerator.h"
#include "Engine/Rendering/RenderServices.h"
#include "Logger/Logger.h"
#include "TileMapECS/TileMapPixelGrid.h"

#include <cstring>

namespace GameEngine::Editor
{

namespace
{

// The loaded texture's pixel dimensions, read from the asset cache only: the
// factory never loads, so it never waits for an import or a cook. Null when the
// texture is not loaded.
const TextureAsset* FindLoadedTexture(AssetManager& assets, const GUID& texGuid, SharedPtr<Asset>& outHolder)
{
    outHolder = assets.GetAsset(texGuid);
    return dynamic_cast<const TextureAsset*>(outHolder.get());
}

// 2D views: a Sprite2D quad in the entity's XY plane at the scene's tile density (one unit per
// texel in a scene without tiles), so the sprite matches the tiles around it and joins tile layers
// and other sprites in the 2D pass's world-Z order.
void AddSprite2D(ECS::World& world, ECS::EntityHandle entity, const GUID& texGuid, const TextureAsset& texture,
                 const Mathematics::Vector3& worldPos)
{
    Components::Transform xf{};
    xf.SetIdentity();
    xf.Position = Mathematics::WorldPosition::FromVector3(worldPos);
    world.AddComponentImmediate(entity, xf);

    const float width = static_cast<float>(texture.GetWidth());
    const SpritePlacementSize size = ResolveSpritePlacementSize(
        width, static_cast<float>(texture.GetHeight()), TileMapECS::ResolveScenePixelsPerUnit(world));
    Components::Sprite2D sprite{};
    sprite.Texture.Set(texGuid);
    sprite.PixelsPerUnit = width / size.Width;
    world.AddComponentImmediate(entity, sprite);
}

// 3D views: a plane primitive scaled to the texture's pixel size, one unit per texel, with a fresh
// .material binding the texture to albedoMap, so the card lights, shadows and depth-tests with the scene.
void AddTexturedPlane(ECS::World& world, ECS::EntityHandle entity, Engine::Renderer::RenderServices& rs,
                      const GUID& texGuid, const std::filesystem::path& texturePath, const TextureAsset& texture,
                      const Mathematics::Vector3& worldPos)
{
    Components::Transform xf{};
    xf.SetIdentity();
    xf.SetScale(static_cast<float>(texture.GetWidth()), 1.0f, static_cast<float>(texture.GetHeight()));
    xf.Position = Mathematics::WorldPosition::FromVector3(worldPos);
    world.AddComponentImmediate(entity, xf);

    const GUID meshGuid = Engine::Renderer::PrimitiveGenerator::PlaneSpriteUvGuid();
    const GUID defaultMatGuid = Engine::Renderer::PrimitiveGenerator::DefaultMaterialGuid();
    world.AddComponentImmediate(entity,
                                Engine::Renderer::PrimitiveGenerator::MakePrimitiveMeshRenderer(&rs, meshGuid,
                                                                                                 defaultMatGuid));
    world.AddComponentImmediate(entity, Engine::Renderer::PrimitiveGenerator::MakePrimitiveLocalBounds(&rs, meshGuid));

    // DefaultMaterialGuid is not file-backed, so the assign writes a fresh .material under
    // <project>/Assets/Materials/, repoints the MeshRenderer and uploads the binding to the runtime Material.
    if (!AssignTextureToEntityMaterialSlot(world, entity, texGuid, "albedoMap", nullptr))
        Logger::Log::Warning("SpriteEntityFactory: failed to bind texture '{}' to the new plane's material",
                             texturePath.string());
}

} // namespace

ECS::EntityHandle CreateSpriteEntityFromTexture(
    ECS::World& world,
    Engine::Renderer::RenderServices& rs,
    AssetManager& assets,
    const std::filesystem::path& texturePath,
    const Mathematics::Vector3& worldPos,
    bool orientForCamera2D)
{
    if (texturePath.empty())
        return {};

    const GUID texGuid = assets.ResolveAssetGuid(texturePath);
    if (texGuid.IsNull())
        return {};

    SharedPtr<Asset> textureHolder;
    const TextureAsset* texture = FindLoadedTexture(assets, texGuid, textureHolder);
    if (!texture)
    {
        Logger::Log::Warning("SpriteEntityFactory: texture '{}' is not loaded, so no sprite was made; load it first "
                             "(RunWhenAssetLoaded) and create the sprite once it lands",
                             texturePath.string());
        return {};
    }
    if (texture->GetWidth() == 0 || texture->GetHeight() == 0)
    {
        Logger::Log::Warning("SpriteEntityFactory: texture '{}' is loaded but has no size ({}x{}), so no sprite was "
                             "made; check that the image file decodes",
                             texturePath.string(), texture->GetWidth(), texture->GetHeight());
        return {};
    }

    ECS::EntityHandle e = world.CreateEntity();
    if (!e.IsValid())
        return {};

    Components::Name nm{};
    std::memset(nm.value, 0, sizeof(nm.value));
    const std::string stem = texturePath.stem().string();
    std::strncpy(nm.value, stem.c_str(), sizeof(nm.value) - 1);
    world.AddComponentImmediate(e, nm);

    if (orientForCamera2D)
        AddSprite2D(world, e, texGuid, *texture, worldPos);
    else
        AddTexturedPlane(world, e, rs, texGuid, texturePath, *texture, worldPos);

    return e;
}

} // namespace GameEngine::Editor
