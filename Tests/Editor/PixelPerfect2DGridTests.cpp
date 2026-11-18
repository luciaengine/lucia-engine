#include "SceneView/PixelPerfect2D/PixelPerfect2DGrid.h"

#include <gtest/gtest.h>

#include <cmath>

using namespace GameEngine::Editor;

namespace
{
// A 16 px tile on a one-unit cell.
constexpr float kTilePixelsPerUnit = 16.0f;
} // namespace

TEST(PixelPerfect2DGrid, OneToOneShowsOneTexelPerRenderPixel)
{
    const PixelPerfect2DGrid grid = ComputePixelPerfect2DGrid(720, 1, kTilePixelsPerUnit);
    EXPECT_FLOAT_EQ(grid.WorldUnitsPerTexel, 1.0f / 16.0f);
    EXPECT_FLOAT_EQ(grid.OrthoHeight, 45.0f);
}

TEST(PixelPerfect2DGrid, EveryScaleShowsTheRenderHeightOverTheScaleInTexels)
{
    for (int32_t scale : {1, 2, 4, 8, 16, 32, 64})
    {
        const PixelPerfect2DGrid grid = ComputePixelPerfect2DGrid(720, scale, kTilePixelsPerUnit);
        EXPECT_FLOAT_EQ(grid.OrthoHeight / grid.WorldUnitsPerTexel, 720.0f / static_cast<float>(scale))
            << "scale " << scale;
    }
}

TEST(PixelPerfect2DGrid, MissingDensityUsesOneTexelPerWorldUnit)
{
    for (float density : {0.0f, -4.0f, std::nanf("")})
    {
        const PixelPerfect2DGrid grid = ComputePixelPerfect2DGrid(720, 2, density);
        EXPECT_FLOAT_EQ(grid.WorldUnitsPerTexel, 1.0f);
        EXPECT_FLOAT_EQ(grid.OrthoHeight, 360.0f);
    }
}

TEST(PixelPerfect2DGrid, ScaleIsClampedAndZeroHeightGivesNoOrthoHeight)
{
    EXPECT_FLOAT_EQ(ComputePixelPerfect2DGrid(720, -4, kTilePixelsPerUnit).OrthoHeight,
                    ComputePixelPerfect2DGrid(720, kPixelPerfectScaleMin, kTilePixelsPerUnit).OrthoHeight);
    EXPECT_FLOAT_EQ(ComputePixelPerfect2DGrid(720, 4096, kTilePixelsPerUnit).OrthoHeight,
                    ComputePixelPerfect2DGrid(720, kPixelPerfectScaleMax, kTilePixelsPerUnit).OrthoHeight);
    const PixelPerfect2DGrid grid = ComputePixelPerfect2DGrid(0, 4, kTilePixelsPerUnit);
    EXPECT_FLOAT_EQ(grid.OrthoHeight, 0.0f);
    EXPECT_FLOAT_EQ(grid.WorldUnitsPerTexel, 1.0f / 16.0f);
}

TEST(PixelPerfect2DGrid, FramingSnapsToThePowerOfTwoStepThatCovers)
{
    // 720 px at 16 px per unit: 45 units at 1x, 22.5 at 2x, 11.25 at 4x.
    EXPECT_EQ(SnapPixelPerfectScaleToHeight(720, 45.0f, kTilePixelsPerUnit), 1);
    EXPECT_EQ(SnapPixelPerfectScaleToHeight(720, 23.0f, kTilePixelsPerUnit), 1);
    EXPECT_EQ(SnapPixelPerfectScaleToHeight(720, 22.5f, kTilePixelsPerUnit), 2);
    EXPECT_EQ(SnapPixelPerfectScaleToHeight(720, 11.25f, kTilePixelsPerUnit), 4);
    EXPECT_EQ(SnapPixelPerfectScaleToHeight(720, 0.01f, kTilePixelsPerUnit), kPixelPerfectScaleMax);
    EXPECT_EQ(SnapPixelPerfectScaleToHeight(0, 10.0f, kTilePixelsPerUnit), kPixelPerfectScaleMin);
    EXPECT_EQ(SnapPixelPerfectScaleToHeight(720, 0.0f, kTilePixelsPerUnit), kPixelPerfectScaleMin);
}

TEST(PixelPerfect2DGrid, ScaleStepsDoubleAndHalveWithinTheOfferedSteps)
{
    EXPECT_EQ(StepPixelPerfectScale(2, 1), 4);
    EXPECT_EQ(StepPixelPerfectScale(2, 3), 16);
    EXPECT_EQ(StepPixelPerfectScale(8, -2), 2);
    EXPECT_EQ(StepPixelPerfectScale(32, 5), kPixelPerfectScaleMax);
    EXPECT_EQ(StepPixelPerfectScale(2, -5), kPixelPerfectScaleMin);
    EXPECT_EQ(StepPixelPerfectScale(4, 0), 4);
}

TEST(PixelPerfect2DGrid, SnapPutsTheWindowMinimumEdgeOnATexelBoundary)
{
    constexpr float kTexel = 1.0f / 16.0f;
    // An odd texel count across the window: a snapped center would leave the edge on a half texel.
    const float halfW = 10.5f * kTexel;
    const float halfH = 5.5f * kTexel;
    double x = 3.14159;
    double y = -2.71828;
    SnapOrthoWindowToTexelGrid(x, y, halfW, halfH, kTexel);

    const double minX = (x - halfW) / kTexel;
    const double minY = (y - halfH) / kTexel;
    EXPECT_NEAR(minX, std::round(minX), 1e-9);
    EXPECT_NEAR(minY, std::round(minY), 1e-9);
    EXPECT_LE(std::abs(x - 3.14159), kTexel * 0.5 + 1e-9);
    EXPECT_LE(std::abs(y + 2.71828), kTexel * 0.5 + 1e-9);

    const double snappedX = x;
    const double snappedY = y;
    SnapOrthoWindowToTexelGrid(x, y, halfW, halfH, kTexel);
    EXPECT_NEAR(x, snappedX, 1e-12);
    EXPECT_NEAR(y, snappedY, 1e-12);
}

TEST(PixelPerfect2DGrid, SnapWithoutATexelSizeLeavesTheCenter)
{
    double x = 1.234;
    double y = 5.678;
    SnapOrthoWindowToTexelGrid(x, y, 4.0f, 3.0f, 0.0f);
    EXPECT_DOUBLE_EQ(x, 1.234);
    EXPECT_DOUBLE_EQ(y, 5.678);
}
