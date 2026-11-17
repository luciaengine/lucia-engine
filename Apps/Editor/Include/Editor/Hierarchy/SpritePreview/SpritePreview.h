#pragma once

#include "ECS/ECS.h"
#include "Mathematics/Vector4.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace GameEngine
{
class AssetManager;
class IThumbnailProvider;
class TreeView;
class UIElement;

namespace Editor
{

/// What a Hierarchy row asks the thumbnail provider for to show a 2D sprite: the atlas region of a Sprite2D or of a
/// sprite plane, or the first frame of the current animation of an AnimatedSprite2D.
struct SpritePreviewRequest
{
    enum class Kind : std::uint8_t
    {
        None,
        AtlasRegion,
        SpriteFrame,
    };

    Kind Source = Kind::None;
    /// The texture (AtlasRegion) or the .spriteframes asset (SpriteFrame), as an absolute path.
    std::filesystem::path AssetPath;
    /// AtlasRegion: u0, v0, u1, v1 in texture space; a flipped sprite swaps the pair of its axis.
    Mathematics::Vector4 Uv{0.0f, 0.0f, 1.0f, 1.0f};
    /// SpriteFrame: the animation the entity plays, and its facing.
    std::string AnimationName;
    bool FlipX = false;
};

/// The preview request of `entity`, or a request of Kind::None when the entity shows no sprite or its asset is not
/// registered. An AnimatedSprite2D wins over a Sprite2D on the same entity.
SpritePreviewRequest ResolveSpritePreviewRequest(ECS::World& world, ECS::EntityHandle entity, AssetManager& assets);

/// Asks `thumbnails` for the crop of `request`. Returns the cached path when it exists; otherwise `onReady` answers
/// once, possibly on a background thread.
std::string RequestSpritePreview(IThumbnailProvider& thumbnails, const SpritePreviewRequest& request,
                                 std::function<void(const std::string& path)> onReady);

/// Shows the sprite crop of `entity` in `rowIcon`, the preview icon of the row that `tree` binds to `rowId`. A crop
/// that arrives later goes into the icon of the row that `tree` then holds bound to `rowId`, if any. Returns false
/// when the entity shows no sprite: the row keeps its glyph. UI thread only.
bool ShowHierarchySpriteRowPreview(TreeView& tree, std::uint64_t rowId, UIElement& rowIcon, ECS::World& world,
                                   ECS::EntityHandle entity, AssetManager& assets, IThumbnailProvider& thumbnails);

} // namespace Editor
} // namespace GameEngine
