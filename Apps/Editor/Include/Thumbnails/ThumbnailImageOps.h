#pragma once

#include "AssetCore/Types.h"

#include <vector>

namespace GameEngine
{
namespace Editor
{

/** RGBA8 image, row-major from the top-left. Used for both source and target. */
struct ThumbnailImage
{
    int32 Width = 0;
    int32 Height = 0;
    std::vector<uint8> Pixels;

    bool IsEmpty() const { return Width <= 0 || Height <= 0 || Pixels.empty(); }
};

/** Normalized sub-rect of a source image. V is image-space: V0 is the top edge. */
struct ThumbnailCropRect
{
    float32 U0 = 0.0f;
    float32 V0 = 0.0f;
    float32 U1 = 1.0f;
    float32 V1 = 1.0f;
};

/** Fully transparent RGBA8 image of the given size. Empty for non-positive extents. */
ThumbnailImage MakeTransparent(int32 width, int32 height);

/**
 * Shrinks `uv` to the texels inside it whose alpha exceeds `alphaThreshold`.
 *
 * Packed sprite frames carry a lot of empty canvas — a frame is sized for the
 * animation's widest pose, so a narrow one floats in transparency. Cropping the
 * rect verbatim spends most of a preview on nothing. Returns `uv` unchanged when
 * the region holds no opaque texel, so a blank frame degrades to the old
 * framing rather than to an empty image.
 *
 * PREVIEWS ONLY. In-scene sprite geometry and pivots are defined against the
 * untrimmed frame; trimming there would move every sprite.
 */
ThumbnailCropRect TrimToOpaqueBounds(const ThumbnailImage& source, const ThumbnailCropRect& uv,
                                     uint8 alphaThreshold);

/**
 * Replicates each texel a whole number of times until the long edge reaches
 * `minEdgePx`, never passing `maxEdgePx`.
 *
 * A trimmed frame can be smaller than the box that shows it, and letting the
 * sampler stretch it there is exactly the blur point-sampling was chosen to
 * avoid. An integer factor keeps every source texel a crisp square block.
 */
ThumbnailImage ScaleUpInteger(const ThumbnailImage& image, int32 minEdgePx, int32 maxEdgePx);

/**
 * Grows the image by `marginPx` transparent texels on every side.
 *
 * A trimmed preview touches its own edges, so in a row it would collide with the
 * label beside it. The breathing room is baked into the image because the icon
 * box sizes the artwork with `contain`, which fits to the box and leaves no gap
 * of its own.
 */
ThumbnailImage AddTransparentMargin(const ThumbnailImage& image, int32 marginPx);

/** Mirrors the image about its vertical axis, in place. */
void MirrorHorizontal(ThumbnailImage& image);

/**
 * Lifts `uv` out of `source` at up to `maxEdgePx`, preserving the sub-rect's
 * aspect ratio and never magnifying — pixel art blown up only adds blur, not
 * detail — and point-sampling on the way down so it does not soften either.
 *
 * Empty when the source is empty or the rect covers no whole texel.
 */
ThumbnailImage CropNearest(const ThumbnailImage& source, const ThumbnailCropRect& uv,
                           int32 maxEdgePx);

} // namespace Editor
} // namespace GameEngine
