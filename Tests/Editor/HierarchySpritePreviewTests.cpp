// The Hierarchy sprite row preview: a Sprite2D row asks for the crop of its atlas region, an AnimatedSprite2D row for
// the first frame of its animation, and a cached crop goes into the row icon at once.
#include <gtest/gtest.h>

#include "Editor/Hierarchy/SpritePreview/SpritePreview.h"

#include "AssetCore/AssetTypes.h"
#include "AssetCore/GUID.h"
#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Components/Animation/AnimatedSprite2D.h"
#include "Components/Rendering/Sprite2D.h"
#include "Core/Application.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Thumbnails/IThumbnailProvider.h"
#include "UI/Controls/TreeView.h"
#include "UI/UIElement.h"

#include <array>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

using namespace GameEngine;
using Editor::SpritePreviewRequest;

namespace
{

AssetManager& StartedAssets()
{
    EngineCore& engine = EngineCore::GetInstance();
    if (!engine.IsInitialized())
    {
        ApplicationConfig config{};
        config.AssetDirectory = ".";
        config.WorkspaceDirectory = ".";
        config.EnableEditor = true;
        EXPECT_TRUE(engine.Initialize(config));
    }
    return engine.GetAssetManager();
}

// An asset in the registry, with no file behind it: the request reads metadata only.
GUID RegisterAsset(AssetManager& assets, AssetType type, const std::string& fileName)
{
    AssetMetadata meta{};
    meta.Guid = GUID::Generate();
    meta.Type = type;
    meta.Name = std::filesystem::path(fileName).stem().string();
    meta.Extension = std::filesystem::path(fileName).extension().string();
    meta.Path = assets.GetRegistry().GetAssetRoot() / "Sprites" / fileName;
    EXPECT_TRUE(assets.GetRegistry().RegisterAssetMetadata(meta));
    return meta.Guid;
}

// Records the crop a row asks for, and answers with `CachedPath`.
class RecordingThumbnails final : public IThumbnailProvider
{
public:
    void SetAssetsRoot(const std::filesystem::path&) override {}
    void RegisterHandler(AssetType, std::unique_ptr<AssetThumbnailHandler>) override {}
    std::string GetOrRequest(const std::filesystem::path&, int, std::function<void(const std::string&)>, bool) override
    {
        return {};
    }

    std::string GetOrRequestAtlasRegion(const std::filesystem::path& texturePath, const std::array<float, 4>& uv,
                                        std::function<void(const std::string&)>) override
    {
        ++AtlasRegionRequests;
        RequestedPath = texturePath;
        RequestedUv = uv;
        return CachedPath;
    }

    std::string GetOrRequestSpriteFrame(const std::filesystem::path& spriteFramesPath, const std::string& animationName,
                                        bool flipX, std::function<void(const std::string&)>) override
    {
        ++SpriteFrameRequests;
        RequestedPath = spriteFramesPath;
        RequestedAnimation = animationName;
        RequestedFlipX = flipX;
        return CachedPath;
    }

    std::string CachedPath;
    int AtlasRegionRequests = 0;
    int SpriteFrameRequests = 0;
    std::filesystem::path RequestedPath;
    std::array<float, 4> RequestedUv{};
    std::string RequestedAnimation;
    bool RequestedFlipX = false;
};

class HierarchySpritePreviewTest : public ::testing::Test
{
protected:
    bool ShowRowPreview(ECS::EntityHandle entity)
    {
        return Editor::ShowHierarchySpriteRowPreview(tree, kRowId, icon, world, entity, assets, thumbnails);
    }

    static constexpr std::uint64_t kRowId = 7;
    AssetManager& assets = StartedAssets();
    ECS::World world;
    TreeView tree;
    UIElement icon;
    RecordingThumbnails thumbnails;
};

} // namespace

TEST_F(HierarchySpritePreviewTest, ASpriteRowAsksForTheCropOfItsAtlasRegion)
{
    Components::Sprite2D sprite{};
    sprite.Texture.Set(RegisterAsset(assets, AssetType::Texture, "hero.png"));
    sprite.U0 = 0.25f;
    sprite.V0 = 0.5f;
    sprite.U1 = 0.5f;
    sprite.V1 = 0.75f;
    sprite.FlipX = true;
    const ECS::EntityHandle entity = world.CreateEntity();
    world.AddComponentImmediate(entity, sprite);

    ASSERT_TRUE(ShowRowPreview(entity));

    EXPECT_EQ(thumbnails.AtlasRegionRequests, 1);
    EXPECT_EQ(thumbnails.RequestedPath.filename(), "hero.png");
    // A flipped sprite swaps its u pair, so the crop faces the way the sprite draws.
    EXPECT_EQ(thumbnails.RequestedUv, (std::array<float, 4>{0.5f, 0.5f, 0.25f, 0.75f}));
}

TEST_F(HierarchySpritePreviewTest, AnAnimatedSpriteRowAsksForTheFrameOfItsAnimation)
{
    Components::AnimatedSprite2D sprite{};
    sprite.SpriteFrames.Set(RegisterAsset(assets, AssetType::SpriteFrames, "hero.spriteframes"));
    std::strncpy(sprite.AnimationName, "Run", sizeof(sprite.AnimationName) - 1);
    sprite.FlipX = true;
    const ECS::EntityHandle entity = world.CreateEntity();
    world.AddComponentImmediate(entity, sprite);

    ASSERT_TRUE(ShowRowPreview(entity));

    EXPECT_EQ(thumbnails.SpriteFrameRequests, 1);
    EXPECT_EQ(thumbnails.AtlasRegionRequests, 0);
    EXPECT_EQ(thumbnails.RequestedPath.filename(), "hero.spriteframes");
    EXPECT_EQ(thumbnails.RequestedAnimation, "Run");
    EXPECT_TRUE(thumbnails.RequestedFlipX);
}

TEST_F(HierarchySpritePreviewTest, ACachedCropGoesIntoTheRowIconAtOnce)
{
    Components::Sprite2D sprite{};
    sprite.Texture.Set(RegisterAsset(assets, AssetType::Texture, "coin.png"));
    const ECS::EntityHandle entity = world.CreateEntity();
    world.AddComponentImmediate(entity, sprite);
    thumbnails.CachedPath = "Thumbnails/coin-crop.png";

    ASSERT_TRUE(ShowRowPreview(entity));

    EXPECT_TRUE(icon.HasClass("hierarchy-entity-model-thumb"));
}

TEST_F(HierarchySpritePreviewTest, ARowWithoutASpriteKeepsItsGlyph)
{
    const ECS::EntityHandle entity = world.CreateEntity();

    EXPECT_FALSE(ShowRowPreview(entity));
    EXPECT_EQ(thumbnails.AtlasRegionRequests + thumbnails.SpriteFrameRequests, 0);
    EXPECT_FALSE(icon.HasClass("hierarchy-entity-model-thumb"));
}
