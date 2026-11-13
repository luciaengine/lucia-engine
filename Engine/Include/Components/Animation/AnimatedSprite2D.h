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
    // @ge-tooltip Animation to play, by name within the .spriteframes asset.
    char AnimationName[kAnimationNameCapacity] = {};

    /** Playback clock, normalized into the animation's own range every frame. */
    // @ge-tooltip Playback clock; wrapped or clamped into the animation's own range.
    float32 TimeSeconds = 0.0f;

    /** Playback rate multiplier; negative plays the strip backwards. */
    // @ge-range -4.0 4.0
    // @ge-tooltip Playback rate multiplier. Negative plays the strip backwards.
    float32 SpeedScale = 1.0f;

    /** Source texels per world unit. Larger = smaller sprite. */
    // @ge-range 1.0 256.0
    // @ge-tooltip Source texels per world unit. A 32x48 frame at 16 covers 2x3 world units.
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
    // @ge-range 0.0 1.0
    // @ge-tooltip Horizontal anchor in normalized frame space: 0 left, 0.5 center, 1 right. Flips mirror around it.
    float32 PivotX = 0.5f;
    // @ge-range 0.0 1.0
    // @ge-tooltip Vertical anchor in normalized frame space: 0 = feet (bottom edge on the entity position), 0.5 = center. Use 0 so frames of differing height keep the feet planted.
    float32 PivotY = 0.5f;

    /** Linear color multiplied into every texel. */
    // @ge-tooltip Linear color multiplied into every texel of the frame.
    ColorLinear Tint{1.0f, 1.0f, 1.0f, 1.0f};

    uint32 RenderLayers = 0xFFFFFFFFu;
    bool Playing = true;
    bool Visible = true;

    /** Mirror the frame about its local Y / X axis (facing, without a transform flip). */
    // @ge-tooltip Mirror the frame horizontally, without flipping the transform.
    bool FlipX = false;
    bool FlipY = false;

    // [DoNotSerialize] Events from the most recent playback update, consumed by SpriteAnimationApi.
    uint32 PendingPlaybackEvents = 0;
    // [DoNotSerialize] Final frame of the update that produced PendingPlaybackEvents.
    uint32 PlaybackEventFrame = 0;
    // [DoNotSerialize] Signed loop count of the update that produced PendingPlaybackEvents.
    int64 PlaybackEventLoops = 0;
    // [DoNotSerialize] Clock captured when PendingPlaybackEvents was published.
    float32 PlaybackEventTimeSeconds = 0;

    void SetAnimationName(std::string_view value)
    {
        CopyToFixedString(AnimationName, value);
    }
    std::string_view GetAnimationName() const { return FixedStringView(AnimationName); }
};

} // namespace Components
} // namespace GameEngine
