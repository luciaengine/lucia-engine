#include "Editor/SceneViewPixelPerfect.h"

#include <gtest/gtest.h>

#include <cmath>

using GameEngine::Editor::ComputePixelPerfect2DGrid;
using GameEngine::Editor::kPixelPerfectScaleMax;
using GameEngine::Editor::PixelPerfect2DGrid;
using GameEngine::Editor::SnapOrthoWindowToTexelGrid;
using GameEngine::Editor::SnapPixelPerfectScaleToHeight;

namespace
{
// The tilemap demo's density: a 16 px tile authored on a one-unit cell.
constexpr float kTilePixelsPerUnit = 16.0f;
} // namespace

TEST(SceneViewPixelPerfect, OneToOneShowsOneTexelPerRenderPixel)
{
    const PixelPerfect2DGrid grid = ComputePixelPerfect2DGrid(720, 1, kTilePixelsPerUnit);
    EXPECT_FLOAT_EQ(grid.WorldUnitsPerTexel, 1.0f / 16.0f);
    // 720 render pixels of a 16 px/unit tilemap is 45 world units tall.
    EXPECT_FLOAT_EQ(grid.OrthoHeight, 45.0f);
}

TEST(SceneViewPixelPerfect, IntegerScaleMultipliesTexelSize)
{
    for (int32_t scale : {1, 2, 4, 8, 16, 32, 64})
    {
        const PixelPerfect2DGrid grid = ComputePixelPerfect2DGrid(720, scale, kTilePixelsPerUnit);
        // Every render pixel column must belong to exactly one texel: the
        // visible texel count is the render height divided by the scale.
        const float visibleTexels = grid.OrthoHeight / grid.WorldUnitsPerTexel;
        EXPECT_FLOAT_EQ(visibleTexels, 720.0f / static_cast<float>(scale)) << "scale " << scale;
    }
}

TEST(SceneViewPixelPerfect, UnitDensityKeepsWorldUnitPerPixelMeaning)
{
    const PixelPerfect2DGrid grid = ComputePixelPerfect2DGrid(720, 2, 1.0f);
    EXPECT_FLOAT_EQ(grid.WorldUnitsPerTexel, 1.0f);
    EXPECT_FLOAT_EQ(grid.OrthoHeight, 360.0f);
}

TEST(SceneViewPixelPerfect, NonPositiveDensityFallsBackToOneTexelPerUnit)
{
    const PixelPerfect2DGrid grid = ComputePixelPerfect2DGrid(720, 1, 0.0f);
    EXPECT_FLOAT_EQ(grid.WorldUnitsPerTexel, 1.0f);
    EXPECT_FLOAT_EQ(grid.OrthoHeight, 720.0f);
}

TEST(SceneViewPixelPerfect, ScaleIsClampedToTheOfferedSteps)
{
    const PixelPerfect2DGrid low = ComputePixelPerfect2DGrid(720, -4, kTilePixelsPerUnit);
    EXPECT_FLOAT_EQ(low.OrthoHeight, ComputePixelPerfect2DGrid(720, 1, kTilePixelsPerUnit).OrthoHeight);

    const PixelPerfect2DGrid high = ComputePixelPerfect2DGrid(720, 4096, kTilePixelsPerUnit);
    EXPECT_FLOAT_EQ(
        high.OrthoHeight,
        ComputePixelPerfect2DGrid(720, kPixelPerfectScaleMax, kTilePixelsPerUnit).OrthoHeight);
}

TEST(SceneViewPixelPerfect, ZeroRenderHeightYieldsNoOrthoHeight)
{
    const PixelPerfect2DGrid grid = ComputePixelPerfect2DGrid(0, 4, kTilePixelsPerUnit);
    EXPECT_FLOAT_EQ(grid.OrthoHeight, 0.0f);
    EXPECT_FLOAT_EQ(grid.WorldUnitsPerTexel, 1.0f / 16.0f);
}

TEST(SceneViewPixelPerfect, FramingSnapsToThePowerOfTwoStepThatCovers)
{
    // 720 px at 16 px/unit is 45 units at 1x, 22.5 at 2x, 11.25 at 4x.
    EXPECT_EQ(SnapPixelPerfectScaleToHeight(720, 45.0f, kTilePixelsPerUnit), 1);
    EXPECT_EQ(SnapPixelPerfectScaleToHeight(720, 23.0f, kTilePixelsPerUnit), 1);
    EXPECT_EQ(SnapPixelPerfectScaleToHeight(720, 22.5f, kTilePixelsPerUnit), 2);
    EXPECT_EQ(SnapPixelPerfectScaleToHeight(720, 11.25f, kTilePixelsPerUnit), 4);
    EXPECT_EQ(SnapPixelPerfectScaleToHeight(720, 0.01f, kTilePixelsPerUnit), kPixelPerfectScaleMax);
}

TEST(SceneViewPixelPerfect, FramingDegradesToOneWhenNothingIsKnown)
{
    EXPECT_EQ(SnapPixelPerfectScaleToHeight(0, 10.0f, kTilePixelsPerUnit), 1);
    EXPECT_EQ(SnapPixelPerfectScaleToHeight(720, 0.0f, kTilePixelsPerUnit), 1);
}

TEST(SceneViewPixelPerfect, SnapPutsTheWindowMinimumEdgeOnATexelBoundary)
{
    constexpr float kTexel = 1.0f / 16.0f;
    // An odd texel count across the window: snapping the CENTER would leave the
    // edge on a half texel, so the edge is what the snap has to land.
    const float halfW = 10.5f * kTexel;
    const float halfH = 5.5f * kTexel;
    double x = 3.14159;
    double y = -2.71828;
    SnapOrthoWindowToTexelGrid(x, y, halfW, halfH, kTexel);

    const double minX = x - halfW;
    const double minY = y - halfH;
    EXPECT_NEAR(std::abs(minX / kTexel - std::round(minX / kTexel)), 0.0, 1e-9);
    EXPECT_NEAR(std::abs(minY / kTexel - std::round(minY / kTexel)), 0.0, 1e-9);
    // The snap never moves the camera by more than half a texel.
    EXPECT_LE(std::abs(x - 3.14159), kTexel * 0.5 + 1e-9);
    EXPECT_LE(std::abs(y + 2.71828), kTexel * 0.5 + 1e-9);
}

TEST(SceneViewPixelPerfect, SnapIsIdempotent)
{
    constexpr float kTexel = 1.0f / 16.0f;
    double x = 0.3;
    double y = 7.9;
    SnapOrthoWindowToTexelGrid(x, y, 4.0f, 3.0f, kTexel);
    const double firstX = x;
    const double firstY = y;
    SnapOrthoWindowToTexelGrid(x, y, 4.0f, 3.0f, kTexel);
    EXPECT_NEAR(x, firstX, 1e-12);
    EXPECT_NEAR(y, firstY, 1e-12);
}

TEST(SceneViewPixelPerfect, SnapIsInertWithoutATexelSize)
{
    double x = 1.234;
    double y = 5.678;
    SnapOrthoWindowToTexelGrid(x, y, 4.0f, 3.0f, 0.0f);
    EXPECT_DOUBLE_EQ(x, 1.234);
    EXPECT_DOUBLE_EQ(y, 5.678);
}
