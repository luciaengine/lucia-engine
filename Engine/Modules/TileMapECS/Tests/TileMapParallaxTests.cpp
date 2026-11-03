#include "TileMapECS/TileMapParallax.h"

#include <gtest/gtest.h>

using namespace GameEngine;
using namespace GameEngine::TileMapECS;

TEST(TileMapParallax, FactorOneIsWorldLockedIdentity)
{
    // The default. Must be exactly zero — not merely small — so a layer left at
    // 1.0 renders byte-identically to one with no parallax applied at all.
    for (float32 camX : {0.0f, 12.5f, -400.0f, 1.0e6f})
    {
        const ParallaxOffset offset = ComputeParallaxOffset(camX, camX * 0.5f, 1.0f, 1.0f, 0.0f, 0.0f);
        EXPECT_EQ(offset.X, 0.0f) << "camX " << camX;
        EXPECT_EQ(offset.Y, 0.0f) << "camX " << camX;
    }
}

TEST(TileMapParallax, FactorZeroLocksToTheCamera)
{
    const ParallaxOffset offset = ComputeParallaxOffset(37.0f, -12.0f, 0.0f, 0.0f, 0.0f, 0.0f);
    EXPECT_FLOAT_EQ(offset.X, 37.0f);
    EXPECT_FLOAT_EQ(offset.Y, -12.0f);
}

TEST(TileMapParallax, HalfFactorTracksHalfTheCameraMotion)
{
    const ParallaxOffset offset = ComputeParallaxOffset(100.0f, 40.0f, 0.5f, 0.5f, 0.0f, 0.0f);
    EXPECT_FLOAT_EQ(offset.X, 50.0f);
    EXPECT_FLOAT_EQ(offset.Y, 20.0f);
}

TEST(TileMapParallax, AxesAreIndependent)
{
    // A layer may scroll horizontally but stay pinned vertically.
    const ParallaxOffset offset = ComputeParallaxOffset(80.0f, 60.0f, 1.0f, 0.0f, 0.0f, 0.0f);
    EXPECT_EQ(offset.X, 0.0f);
    EXPECT_FLOAT_EQ(offset.Y, 60.0f);
}

TEST(TileMapParallax, ScreenShiftIsProportionalToFactor)
{
    // The property the feature actually sells: for a camera move of `delta`, the
    // layer's motion RELATIVE TO THE CAMERA is -delta * factor, so a 0.5 layer
    // slides exactly half as far on screen as a world-locked 1.0 layer.
    constexpr float32 kCamA = 10.0f;
    constexpr float32 kCamB = 30.0f;
    constexpr float32 kDelta = kCamB - kCamA;

    const auto relativeShift = [](float32 camA, float32 camB, float32 factor)
    {
        const float32 worldA = ComputeParallaxOffset(camA, 0.0f, factor, 1.0f, 0.0f, 0.0f).X;
        const float32 worldB = ComputeParallaxOffset(camB, 0.0f, factor, 1.0f, 0.0f, 0.0f).X;
        // Position relative to the camera, differenced between the two poses.
        return (worldB - camB) - (worldA - camA);
    };

    EXPECT_FLOAT_EQ(relativeShift(kCamA, kCamB, 1.0f), -kDelta);
    EXPECT_FLOAT_EQ(relativeShift(kCamA, kCamB, 0.5f), -kDelta * 0.5f);
    EXPECT_FLOAT_EQ(relativeShift(kCamA, kCamB, 0.0f), 0.0f);

    // Half factor is exactly half the world-locked shift — the measured ratio
    // the runtime screenshot comparison checks.
    EXPECT_FLOAT_EQ(relativeShift(kCamA, kCamB, 0.5f) / relativeShift(kCamA, kCamB, 1.0f), 0.5f);
}

TEST(TileMapParallax, QuantizedLayerSitsOnWholeTexelsRelativeToTheCamera)
{
    // What the pixel grid needs: the layer's distance from the camera is a whole
    // number of texels, so it rasterizes crisp, and that distance only ever
    // steps in the direction the camera moves -- never back by a texel.
    constexpr float32 kTexel = 1.0f / 16.0f;
    constexpr float32 kFactor = 0.25f;
    float32 previousRelative = 0.0f;
    for (int32 step = 0; step <= 64; ++step)
    {
        const float32 camera = 10.0f + static_cast<float32>(step) * kTexel * 0.37f;
        const float32 relative =
            ComputeParallaxOffset(camera, 0.0f, kFactor, 1.0f, kTexel, 0.0f).X - camera;
        const float32 texels = relative / kTexel;
        EXPECT_NEAR(texels, std::round(texels), 1e-3f) << "step " << step;
        if (step > 0)
            EXPECT_LE(relative, previousRelative + 1e-5f) << "step " << step;
        previousRelative = relative;
    }
}

TEST(TileMapParallax, FactorAboveOneOvertakesTheCamera)
{
    // Not clamped: a factor above 1 slides the layer against the camera, which is
    // a usable foreground-rush effect rather than an error to reject.
    const ParallaxOffset offset = ComputeParallaxOffset(10.0f, 10.0f, 2.0f, 1.5f, 0.0f, 0.0f);
    EXPECT_FLOAT_EQ(offset.X, -10.0f);
    EXPECT_FLOAT_EQ(offset.Y, -5.0f);
}

TEST(TileMapParallax, TexelQuantizationSnapsTheLayerToTheGridRelativeToTheCamera)
{
    // 16 px tiles on a one-unit cell: one texel is 1/16 of a world unit.
    constexpr float32 kTexel = 1.0f / 16.0f;
    // Factor 0.5 at camera 10.03 puts the layer 5.015 world units behind the
    // camera on screen, which is 80.24 texels -- it must sit at texel 80 (5.0)
    // from the camera, not between texels.
    const ParallaxOffset offset =
        ComputeParallaxOffset(10.03f, 10.03f, 0.5f, 0.5f, kTexel, kTexel);
    EXPECT_FLOAT_EQ(offset.X - 10.03f, -5.0f);
    EXPECT_FLOAT_EQ(offset.Y - 10.03f, -5.0f);
}

TEST(TileMapParallax, TexelQuantizationNeverMovesMoreThanHalfATexel)
{
    constexpr float32 kTexel = 1.0f / 16.0f;
    for (int step = 0; step < 64; ++step)
    {
        const float32 camX = static_cast<float32>(step) * 0.017f;
        const ParallaxOffset snapped =
            ComputeParallaxOffset(camX, 0.0f, 0.25f, 1.0f, kTexel, kTexel);
        const ParallaxOffset exact = ComputeParallaxOffset(camX, 0.0f, 0.25f, 1.0f, 0.0f, 0.0f);
        EXPECT_LE(std::abs(snapped.X - exact.X), kTexel * 0.5f + 1e-6f) << "step " << step;
        // The layer's distance from the camera is an exact multiple of the texel size.
        const float32 relativeTexels = (snapped.X - camX) / kTexel;
        EXPECT_NEAR(relativeTexels, std::round(relativeTexels), 1e-3f) << "step " << step;
    }
}

TEST(TileMapParallax, WorldLockedLayerIsUnmovedByQuantization)
{
    constexpr float32 kTexel = 1.0f / 16.0f;
    const ParallaxOffset offset =
        ComputeParallaxOffset(123.456f, -78.9f, 1.0f, 1.0f, kTexel, kTexel);
    EXPECT_FLOAT_EQ(offset.X, 0.0f);
    EXPECT_FLOAT_EQ(offset.Y, 0.0f);
}
