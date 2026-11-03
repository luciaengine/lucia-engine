#include "Thumbnails/ThumbnailImageOps.h"

#include <gtest/gtest.h>

using namespace GameEngine;
using namespace GameEngine::Editor;

namespace
{

/** Source whose every texel is a distinct, decodable colour: R = x, G = y. */
ThumbnailImage MakeRamp(int32 width, int32 height)
{
    ThumbnailImage image = MakeTransparent(width, height);
    for (int32 y = 0; y < height; ++y)
    {
        for (int32 x = 0; x < width; ++x)
        {
            const size_t index =
                (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4u;
            image.Pixels[index + 0u] = static_cast<uint8>(x);
            image.Pixels[index + 1u] = static_cast<uint8>(y);
            image.Pixels[index + 2u] = 0u;
            image.Pixels[index + 3u] = 255u;
        }
    }
    return image;
}

/** The source x/y a destination pixel ended up carrying. */
std::pair<int32, int32> SourceAt(const ThumbnailImage& image, int32 x, int32 y)
{
    const size_t index =
        (static_cast<size_t>(y) * static_cast<size_t>(image.Width) + static_cast<size_t>(x)) * 4u;
    return {image.Pixels[index + 0u], image.Pixels[index + 1u]};
}

} // namespace

// A quarter-width strip frame (the shape every sliced sprite sheet produces)
// must yield exactly its own texels, not the neighbouring frames'.
TEST(ThumbnailImageOps, CropTakesOnlyTheRequestedSubRect)
{
    const ThumbnailImage sheet = MakeRamp(16, 8);

    const ThumbnailImage crop = CropNearest(sheet, ThumbnailCropRect{0.25f, 0.0f, 0.5f, 1.0f}, 256);

    ASSERT_EQ(crop.Width, 4);
    ASSERT_EQ(crop.Height, 8);
    EXPECT_EQ(SourceAt(crop, 0, 0), std::make_pair(4, 0));
    EXPECT_EQ(SourceAt(crop, 3, 7), std::make_pair(7, 7));
}

// V is image-space: V0 is the top edge, matching the row order of both images.
TEST(ThumbnailImageOps, CropTreatsV0AsTheTopEdge)
{
    const ThumbnailImage sheet = MakeRamp(4, 8);

    const ThumbnailImage crop = CropNearest(sheet, ThumbnailCropRect{0.0f, 0.0f, 1.0f, 0.5f}, 256);

    ASSERT_EQ(crop.Height, 4);
    EXPECT_EQ(SourceAt(crop, 0, 0).second, 0);
    EXPECT_EQ(SourceAt(crop, 0, 3).second, 3);
}

// Pixel art blown up only adds blur, so a small frame stays its own size even
// when the caller allows a much larger edge.
TEST(ThumbnailImageOps, CropNeverMagnifies)
{
    const ThumbnailImage sheet = MakeRamp(8, 8);

    const ThumbnailImage crop = CropNearest(sheet, ThumbnailCropRect{}, 256);

    EXPECT_EQ(crop.Width, 8);
    EXPECT_EQ(crop.Height, 8);
}

// A downscale fits the long edge and keeps the sub-rect's aspect ratio, so the
// preview is never stretched.
TEST(ThumbnailImageOps, CropFitsTheLongEdgeAndKeepsAspect)
{
    const ThumbnailImage sheet = MakeRamp(64, 32);

    const ThumbnailImage crop = CropNearest(sheet, ThumbnailCropRect{}, 16);

    EXPECT_EQ(crop.Width, 16);
    EXPECT_EQ(crop.Height, 8);
}

TEST(ThumbnailImageOps, CropRejectsDegenerateInput)
{
    const ThumbnailImage sheet = MakeRamp(8, 8);

    EXPECT_TRUE(CropNearest(ThumbnailImage{}, ThumbnailCropRect{}, 16).IsEmpty());
    EXPECT_TRUE(CropNearest(sheet, ThumbnailCropRect{}, 0).IsEmpty());
    // A rect thinner than one texel rounds to an empty span rather than to a
    // one-pixel smear of whatever happens to sit at the edge.
    EXPECT_TRUE(CropNearest(sheet, ThumbnailCropRect{0.5f, 0.0f, 0.5f, 1.0f}, 16).IsEmpty());
}

// FlipX is baked into the cached PNG, so the mirror has to be exact.
TEST(ThumbnailImageOps, MirrorHorizontalReversesEachRow)
{
    ThumbnailImage image = MakeRamp(5, 2);

    MirrorHorizontal(image);

    EXPECT_EQ(SourceAt(image, 0, 0).first, 4);
    EXPECT_EQ(SourceAt(image, 2, 0).first, 2);
    EXPECT_EQ(SourceAt(image, 4, 1).first, 0);
    EXPECT_EQ(SourceAt(image, 0, 1).second, 1);
}

TEST(ThumbnailImageOps, MirrorHorizontalIgnoresEmptyImages)
{
    ThumbnailImage image;
    MirrorHorizontal(image);
    EXPECT_TRUE(image.IsEmpty());
}

namespace
{

/** Fully transparent sheet with one opaque rectangle painted into it. */
ThumbnailImage MakeSheetWithOpaqueBox(int32 width, int32 height, int32 x0, int32 y0, int32 x1,
                                      int32 y1, uint8 alpha = 255)
{
    ThumbnailImage image = MakeTransparent(width, height);
    for (int32 y = y0; y < y1; ++y)
    {
        for (int32 x = x0; x < x1; ++x)
        {
            const size_t index =
                (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4u;
            image.Pixels[index + 0u] = 200u;
            image.Pixels[index + 3u] = alpha;
        }
    }
    return image;
}

} // namespace

// Packed frames float in empty canvas; the preview should frame the art.
TEST(ThumbnailImageOps, TrimFindsTheOpaqueBox)
{
    const ThumbnailImage sheet = MakeSheetWithOpaqueBox(16, 16, 4, 6, 10, 12);

    const ThumbnailCropRect trimmed = TrimToOpaqueBounds(sheet, ThumbnailCropRect{}, 8);

    EXPECT_FLOAT_EQ(trimmed.U0, 4.0f / 16.0f);
    EXPECT_FLOAT_EQ(trimmed.V0, 6.0f / 16.0f);
    // The exclusive edge is one past the last opaque texel.
    EXPECT_FLOAT_EQ(trimmed.U1, 10.0f / 16.0f);
    EXPECT_FLOAT_EQ(trimmed.V1, 12.0f / 16.0f);
}

// The load-bearing one: frames sit side by side in a strip, so a trim that
// scanned the whole sheet would pull in the neighbouring pose.
TEST(ThumbnailImageOps, TrimStaysInsideTheGivenSubRect)
{
    // Art in the left half only; the right half (the "next frame") is empty.
    const ThumbnailImage sheet = MakeSheetWithOpaqueBox(16, 8, 1, 1, 7, 7);

    const ThumbnailCropRect rightHalf{0.5f, 0.0f, 1.0f, 1.0f};
    const ThumbnailCropRect trimmed = TrimToOpaqueBounds(sheet, rightHalf, 8);

    // Nothing opaque inside that half — the rect comes back untouched rather
    // than snapping onto the neighbour's art.
    EXPECT_FLOAT_EQ(trimmed.U0, rightHalf.U0);
    EXPECT_FLOAT_EQ(trimmed.U1, rightHalf.U1);
    EXPECT_FLOAT_EQ(trimmed.V0, rightHalf.V0);
    EXPECT_FLOAT_EQ(trimmed.V1, rightHalf.V1);
}

// Antialiased packing fringes must not drag the bounds back out to the frame.
TEST(ThumbnailImageOps, TrimIgnoresAlphaAtOrBelowTheThreshold)
{
    ThumbnailImage sheet = MakeSheetWithOpaqueBox(16, 16, 6, 6, 10, 10);
    // A single faint texel far from the art.
    const size_t fringe = (static_cast<size_t>(1) * 16u + 1u) * 4u;
    sheet.Pixels[fringe + 3u] = 8u;

    const ThumbnailCropRect trimmed = TrimToOpaqueBounds(sheet, ThumbnailCropRect{}, 8);

    EXPECT_FLOAT_EQ(trimmed.U0, 6.0f / 16.0f);
    EXPECT_FLOAT_EQ(trimmed.V0, 6.0f / 16.0f);
}

TEST(ThumbnailImageOps, TrimReturnsTheRectWhenNothingIsOpaque)
{
    const ThumbnailImage sheet = MakeTransparent(8, 8);

    const ThumbnailCropRect input{0.25f, 0.25f, 0.75f, 0.75f};
    const ThumbnailCropRect trimmed = TrimToOpaqueBounds(sheet, input, 8);

    EXPECT_FLOAT_EQ(trimmed.U0, input.U0);
    EXPECT_FLOAT_EQ(trimmed.V1, input.V1);
}

// Trimmed art can be smaller than the box that shows it; an integer factor
// keeps every texel a crisp block instead of letting the sampler smear it.
TEST(ThumbnailImageOps, ScaleUpUsesAWholeFactorAndReplicatesTexels)
{
    const ThumbnailImage source = MakeRamp(4, 2);

    const ThumbnailImage scaled = ScaleUpInteger(source, 12, 256);

    ASSERT_EQ(scaled.Width, 12);
    ASSERT_EQ(scaled.Height, 6);
    // Every 3x3 block carries one source texel.
    EXPECT_EQ(SourceAt(scaled, 0, 0), std::make_pair(0, 0));
    EXPECT_EQ(SourceAt(scaled, 2, 2), std::make_pair(0, 0));
    EXPECT_EQ(SourceAt(scaled, 3, 0), std::make_pair(1, 0));
    EXPECT_EQ(SourceAt(scaled, 11, 5), std::make_pair(3, 1));
}

TEST(ThumbnailImageOps, ScaleUpLeavesLargeEnoughImagesAlone)
{
    const ThumbnailImage source = MakeRamp(64, 32);

    const ThumbnailImage scaled = ScaleUpInteger(source, 32, 256);

    EXPECT_EQ(scaled.Width, 64);
    EXPECT_EQ(scaled.Height, 32);
}

TEST(ThumbnailImageOps, ScaleUpNeverPassesTheMaxEdge)
{
    const ThumbnailImage source = MakeRamp(10, 5);

    // Wants 10x, but only 6x fits under a 64px cap.
    const ThumbnailImage scaled = ScaleUpInteger(source, 100, 64);

    EXPECT_EQ(scaled.Width, 60);
    EXPECT_EQ(scaled.Height, 30);
}

TEST(ThumbnailImageOps, MarginGrowsEverySideAndKeepsTheArtCentred)
{
    const ThumbnailImage source = MakeRamp(4, 3);

    const ThumbnailImage padded = AddTransparentMargin(source, 2);

    ASSERT_EQ(padded.Width, 8);
    ASSERT_EQ(padded.Height, 7);
    // Corner is empty, and the art starts at the inset.
    const size_t corner = 0u;
    EXPECT_EQ(padded.Pixels[corner + 3u], 0u);
    EXPECT_EQ(SourceAt(padded, 2, 2), std::make_pair(0, 0));
    EXPECT_EQ(SourceAt(padded, 5, 4), std::make_pair(3, 2));
}

TEST(ThumbnailImageOps, MarginOfZeroIsANoOp)
{
    const ThumbnailImage source = MakeRamp(4, 4);

    const ThumbnailImage padded = AddTransparentMargin(source, 0);

    EXPECT_EQ(padded.Width, 4);
    EXPECT_EQ(padded.Height, 4);
}
