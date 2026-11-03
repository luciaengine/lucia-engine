#pragma once

#include "AssetCore/AssetTypes.h"
#include "Components/AssetRef.h"
#include "Types/StringUtils.h"
#include "Types/Types.h"
#include "Types/Color.h"

#include <algorithm>
#include <cstring>
#include <string_view>

namespace GameEngine
{
namespace Components
{

/**
 * @brief Draws one animation of a .spriteframes asset as a quad in the entity's
 * local XY plane, +Y up (the tilemap convention, so sprites and tiles share a
 * frame).
 *
 * The quad is centered on the entity origin and sized from the current frame's
 * pixel extent divided by PixelsPerUnit, so a 32x48 frame at the default 16
 * covers 2x3 world units. The entity transform's local X/Y axes carry rotation
 * and scale, so the usual TRS still applies on top.
 */
struct AnimatedSprite2D
{
    static constexpr uint32 kAnimationNameCapacity = 64;

    AssetRef<AssetType::SpriteFrames> SpriteFrames;

    /** Animation to play, by name within the asset. Empty = nothing renders. */
    char AnimationName[kAnimationNameCapacity] = {};

    /** Playback clock, normalized into the animation's own range every frame. */
    float32 TimeSeconds = 0.0f;

    /** Playback rate multiplier; negative plays the strip backwards. */
    float32 SpeedScale = 1.0f;

    /** Source texels per world unit. Larger = smaller sprite. */
    float32 PixelsPerUnit = 16.0f;

    /**
     * Point of the frame that sits on the entity's Transform position, in
     * normalized frame space: (0,0) bottom-left, (0.5,0.5) center, (0.5,0)
     * feet. A frame carrying its own pivot overrides this.
     *
     * Feet-anchoring is what keeps a character planted when its frames change
     * size between animations: with a centered pivot, a taller attack frame
     * moves the visible feet by half the height difference.
     */
    float32 PivotX = 0.5f;
    float32 PivotY = 0.5f;

    /** Linear color multiplied into every texel. */
    ColorLinear Tint{1.0f, 1.0f, 1.0f, 1.0f};

    uint32 RenderLayers = 0xFFFFFFFFu;
    bool Playing = true;
    bool Visible = true;

    /** Mirror the frame about its local Y / X axis (facing, without a transform flip). */
    bool FlipX = false;
    bool FlipY = false;

    void SetAnimationName(std::string_view value)
    {
        CopyToFixedString(AnimationName, value);
    }
    std::string_view GetAnimationName() const { return FixedStringView(AnimationName); }
};

} // namespace Components
} // namespace GameEngine
