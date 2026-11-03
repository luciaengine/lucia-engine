// Manual reflection + cross-DLL template instantiation for AnimatedSprite2D.
//
// The build-time ComponentScanner skips this struct: its AssetRef<> field is a
// templated type the scanner's lightweight C++ parser can't resolve, so it omits
// the struct rather than emit an incomplete field list. Components with asset
// references are registered by hand (same approach as LensFlareSource and
// TileMapLayerRef).

#include "Components/Animation/AnimatedSprite2D.h"
#include "Components/ComponentRegistration.h" // GE_REGISTER_COMPONENT

#include "ECS/ECSTemplates.h" // GE_INSTANTIATE_ENGINE_COMPONENT

// Field table + factory registration (Add Component menu + scene serialization).
GE_REGISTER_COMPONENT(GameEngine::Components::AnimatedSprite2D, SpriteFrames, AnimationName,
                      TimeSeconds, SpeedScale, PixelsPerUnit, PivotX, PivotY, Tint, Playing,
                      Visible, FlipX, FlipY, RenderLayers);

// Inspector polish: slider bounds + tooltips (must follow registration).
namespace
{
using GameEngine::Components::AnimatedSprite2D;
using GameEngine::Components::SetReflectedFieldRange;
using GameEngine::Components::SetReflectedFieldTooltip;

const bool kAnimatedSprite2DPolish =
    SetReflectedFieldRange<AnimatedSprite2D>("SpeedScale", -4.0f, 4.0f) &&
    SetReflectedFieldRange<AnimatedSprite2D>("PixelsPerUnit", 1.0f, 256.0f) &&
    SetReflectedFieldRange<AnimatedSprite2D>("PivotX", 0.0f, 1.0f) &&
    SetReflectedFieldRange<AnimatedSprite2D>("PivotY", 0.0f, 1.0f) &&
    SetReflectedFieldTooltip<AnimatedSprite2D>(
        "AnimationName", "Animation to play, by name within the .spriteframes asset.") &&
    SetReflectedFieldTooltip<AnimatedSprite2D>(
        "TimeSeconds", "Playback clock; wrapped or clamped into the animation's own range.") &&
    SetReflectedFieldTooltip<AnimatedSprite2D>(
        "SpeedScale", "Playback rate multiplier. Negative plays the strip backwards.") &&
    SetReflectedFieldTooltip<AnimatedSprite2D>(
        "PixelsPerUnit",
        "Source texels per world unit. A 32x48 frame at 16 covers 2x3 world units.") &&
    SetReflectedFieldTooltip<AnimatedSprite2D>(
        "PivotX",
        "Horizontal anchor in normalized frame space: 0 left, 0.5 center, 1 right. "
        "Flips mirror around it.") &&
    SetReflectedFieldTooltip<AnimatedSprite2D>(
        "PivotY",
        "Vertical anchor in normalized frame space: 0 = feet (bottom edge on the entity "
        "position), 0.5 = center. Use 0 so frames of differing height keep the feet planted.") &&
    SetReflectedFieldTooltip<AnimatedSprite2D>(
        "Tint", "Linear color multiplied into every texel of the frame.") &&
    SetReflectedFieldTooltip<AnimatedSprite2D>(
        "FlipX", "Mirror the frame horizontally, without flipping the transform.");
} // namespace

// Explicit instantiation of the type-erased handler + World accessors so the
// component works across the Engine DLL boundary.
namespace GameEngine::ECS
{
GE_INSTANTIATE_ENGINE_COMPONENT(Components::AnimatedSprite2D);
} // namespace GameEngine::ECS
