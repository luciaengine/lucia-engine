#include "Editor/Hierarchy/SpritePreview/SpritePreview.h"

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Components/Animation/AnimatedSprite2D.h"
#include "Components/Rendering/Sprite2D.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Editor/Hierarchy/HierarchyEntity/HierarchyEntityIcon.h"
#include "Thumbnails/IThumbnailProvider.h"
#include "UI/Controls/TreeView.h"
#include "UI/UIElement.h"
#include "UI/UiPostHandle.h"

#include <utility>

namespace GameEngine::Editor
{
namespace
{
// The class HierarchyPanel gives the icon element of each row.
constexpr const char* kRowPreviewIconClass = "hierarchy-preview-icon";

UIElement* FindRowPreviewIcon(UIElement& row)
{
    for (const auto& child : row.GetChildren())
        if (child && child->HasClass(kRowPreviewIconClass))
            return child.get();
    return nullptr;
}

SpritePreviewRequest ResolveSpriteFrameRequest(const Components::AnimatedSprite2D& sprite, AssetManager& assets)
{
    SpritePreviewRequest request;
    AssetMetadata meta;
    if (sprite.SpriteFrames.IsNull() || !assets.GetRegistry().TryGetAssetMetadata(sprite.SpriteFrames.ToGuid(), meta))
        return request;
    request.Source = SpritePreviewRequest::Kind::SpriteFrame;
    request.AssetPath = assets.ResolveAssetPath(meta.Path);
    request.AnimationName = std::string(sprite.GetAnimationName());
    request.FlipX = sprite.FlipX;
    return request;
}

// The crop lands on the UI thread: the row is looked up again, because the view may have rebound it.
void ApplyCropToBoundRow(TreeView& tree, std::uint64_t rowId, const std::string& path)
{
    UIElement* row = tree.FindBoundRow(rowId);
    UIElement* icon = row ? FindRowPreviewIcon(*row) : nullptr;
    if (icon)
        ApplySpriteThumbnail(*icon, path);
}
} // namespace

SpritePreviewRequest ResolveSpritePreviewRequest(ECS::World& world, ECS::EntityHandle entity, AssetManager& assets)
{
    if (!entity.IsValid() || !world.IsValid(entity))
        return {};
    if (const auto* animated = world.GetComponent<Components::AnimatedSprite2D>(entity))
        return ResolveSpriteFrameRequest(*animated, assets);

    SpritePreviewRequest request;
    request.AssetPath = TryResolveSpriteAlbedoTexturePath(&world, entity, &assets);
    if (request.AssetPath.empty())
        return {};
    request.Source = SpritePreviewRequest::Kind::AtlasRegion;
    if (const auto* sprite = world.GetComponent<Components::Sprite2D>(entity))
    {
        request.Uv = {sprite->U0, sprite->V0, sprite->U1, sprite->V1};
        if (sprite->FlipX)
            std::swap(request.Uv[0], request.Uv[2]);
        if (sprite->FlipY)
            std::swap(request.Uv[1], request.Uv[3]);
    }
    return request;
}

std::string RequestSpritePreview(IThumbnailProvider& thumbnails, const SpritePreviewRequest& request,
                                 std::function<void(const std::string& path)> onReady)
{
    switch (request.Source)
    {
    case SpritePreviewRequest::Kind::AtlasRegion:
        return thumbnails.GetOrRequestAtlasRegion(request.AssetPath, request.Uv, std::move(onReady));
    case SpritePreviewRequest::Kind::SpriteFrame:
        return thumbnails.GetOrRequestSpriteFrame(request.AssetPath, request.AnimationName, request.FlipX,
                                                  std::move(onReady));
    case SpritePreviewRequest::Kind::None:
        break;
    }
    return {};
}

bool ShowHierarchySpriteRowPreview(TreeView& tree, std::uint64_t rowId, UIElement& rowIcon, ECS::World& world,
                                   ECS::EntityHandle entity, AssetManager& assets, IThumbnailProvider& thumbnails)
{
    const SpritePreviewRequest request = ResolveSpritePreviewRequest(world, entity, assets);
    if (request.Source == SpritePreviewRequest::Kind::None)
        return false;

    // The provider can answer on a worker thread; the post handle skips the crop when the tree is gone.
    const UI::UiPostHandle post = tree.GetPostHandle();
    TreeView* treePointer = &tree;
    const std::string cached = RequestSpritePreview(
        thumbnails, request,
        [post, treePointer, rowId](const std::string& path)
        {
            if (!path.empty())
                post.Post([treePointer, rowId, path]() { ApplyCropToBoundRow(*treePointer, rowId, path); });
        });
    if (!cached.empty())
        ApplySpriteThumbnail(rowIcon, cached);
    return true;
}

} // namespace GameEngine::Editor
