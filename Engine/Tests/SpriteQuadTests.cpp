#include "Engine/Rendering/2D/SpriteQuad.h"
#include "Engine/Rendering/SpritePixelGrid.h"

#include <gtest/gtest.h>

using namespace GameEngine;
using namespace GameEngine::Engine::Renderer;

namespace
{

// Column-major identity with a world position in column 3.
void MakeTransform(float32* m, float32 x, float32 y, float32 z)
{
    for (int32 i = 0; i < 16; ++i)
        m[i] = 0.0f;
    m[0] = 1.0f;
    m[5] = 1.0f;
    m[10] = 1.0f;
    m[15] = 1.0f;
    m[12] = x;
    m[13] = y;
    m[14] = z;
}

float32 BottomEdgeY(const SpriteQuadPlacement& p) { return p.Center[1] - p.AxisY[1]; }
float32 TopEdgeY(const SpriteQuadPlacement& p) { return p.Center[1] + p.AxisY[1]; }
float32 LeftEdgeX(const SpriteQuadPlacement& p) { return p.Center[0] - p.AxisX[0]; }
float32 RightEdgeX(const SpriteQuadPlacement& p) { return p.Center[0] + p.AxisX[0]; }

constexpr float32 kEps = 1.0e-5f;

} // namespace

TEST(SpriteQuad, CenterPivotPutsTheQuadCentreOnTheEntity)
{
    float32 m[16];
    MakeTransform(m, 3.0f, 7.0f, -2.0f);
    const auto p = BuildSpriteQuad(m, 2.0f, 3.0f, 0.5f, 0.5f, false, false);

    EXPECT_NEAR(p.Center[0], 3.0f, kEps);
    EXPECT_NEAR(p.Center[1], 7.0f, kEps);
    EXPECT_NEAR(p.Center[2], -2.0f, kEps);
    EXPECT_NEAR(p.AxisX[0], 1.0f, kEps);
    EXPECT_NEAR(p.AxisY[1], 1.5f, kEps);
}

// The headline case: pivot (0.5, 0) means "the entity position IS the feet".
TEST(SpriteQuad, FeetPivotPutsTheBottomEdgeOnTheEntity)
{
    float32 m[16];
    MakeTransform(m, 0.0f, 5.0f, 0.0f);
    const auto p = BuildSpriteQuad(m, 2.0f, 3.0f, 0.5f, 0.0f, false, false);

    EXPECT_NEAR(BottomEdgeY(p), 5.0f, kEps);
    EXPECT_NEAR(TopEdgeY(p), 8.0f, kEps);
    EXPECT_NEAR(p.Center[0], 0.0f, kEps);
}

// Regression for the "attack animation lifts the character off the ground" bug:
// a taller frame must grow UPWARD from the feet, never move them.
TEST(SpriteQuad, FeetPivotKeepsTheFeetStillWhenFrameHeightChanges)
{
    float32 m[16];
    MakeTransform(m, 0.0f, 0.0f, 0.0f);

    const auto idle = BuildSpriteQuad(m, 2.0f, 2.0f, 0.5f, 0.0f, false, false);
    const auto attack = BuildSpriteQuad(m, 3.0f, 4.0f, 0.5f, 0.0f, false, false);

    EXPECT_NEAR(BottomEdgeY(idle), BottomEdgeY(attack), kEps);
    EXPECT_NEAR(BottomEdgeY(attack), 0.0f, kEps);
    // The extra height goes entirely into the top edge.
    EXPECT_NEAR(TopEdgeY(idle), 2.0f, kEps);
    EXPECT_NEAR(TopEdgeY(attack), 4.0f, kEps);
}

// The defect this replaces: with a centred pivot, a frame-height change moves
// the feet by half the difference. Pinned so the old behaviour stays a choice.
TEST(SpriteQuad, CenterPivotSinksTheFeetWhenFrameHeightChanges)
{
    float32 m[16];
    MakeTransform(m, 0.0f, 0.0f, 0.0f);

    const auto idle = BuildSpriteQuad(m, 2.0f, 2.0f, 0.5f, 0.5f, false, false);
    const auto attack = BuildSpriteQuad(m, 2.0f, 4.0f, 0.5f, 0.5f, false, false);

    EXPECT_NEAR(BottomEdgeY(idle), -1.0f, kEps);
    EXPECT_NEAR(BottomEdgeY(attack), -2.0f, kEps);
}

TEST(SpriteQuad, FlipXMirrorsAboutThePivotNotTheQuadCentre)
{
    float32 m[16];
    MakeTransform(m, 0.0f, 0.0f, 0.0f);

    // Pivot a quarter in from the left of a 4-unit-wide frame.
    const auto normal = BuildSpriteQuad(m, 4.0f, 2.0f, 0.25f, 0.0f, false, false);
    EXPECT_NEAR(LeftEdgeX(normal), -1.0f, kEps);
    EXPECT_NEAR(RightEdgeX(normal), 3.0f, kEps);

    const auto flipped = BuildSpriteQuad(m, 4.0f, 2.0f, 0.25f, 0.0f, true, false);
    // Exact mirror of the unflipped span about the entity position.
    EXPECT_NEAR(LeftEdgeX(flipped), -3.0f, kEps);
    EXPECT_NEAR(RightEdgeX(flipped), 1.0f, kEps);
}

// Turning around must not translate a character, so a centred pivot is the one
// case where flipping changes nothing about placement.
TEST(SpriteQuad, FlipXWithCentredPivotDoesNotMoveTheQuad)
{
    float32 m[16];
    MakeTransform(m, 2.0f, 0.0f, 0.0f);

    const auto normal = BuildSpriteQuad(m, 4.0f, 2.0f, 0.5f, 0.0f, false, false);
    const auto flipped = BuildSpriteQuad(m, 4.0f, 2.0f, 0.5f, 0.0f, true, false);

    EXPECT_NEAR(normal.Center[0], flipped.Center[0], kEps);
    EXPECT_NEAR(normal.Center[1], flipped.Center[1], kEps);
}

TEST(SpriteQuad, FlipYMirrorsAboutThePivot)
{
    float32 m[16];
    MakeTransform(m, 0.0f, 0.0f, 0.0f);

    // Feet pivot: the quad sits entirely above the entity.
    const auto normal = BuildSpriteQuad(m, 2.0f, 3.0f, 0.5f, 0.0f, false, false);
    EXPECT_NEAR(BottomEdgeY(normal), 0.0f, kEps);
    EXPECT_NEAR(TopEdgeY(normal), 3.0f, kEps);

    // Flipped, it hangs entirely below it.
    const auto flipped = BuildSpriteQuad(m, 2.0f, 3.0f, 0.5f, 0.0f, false, true);
    EXPECT_NEAR(BottomEdgeY(flipped), -3.0f, kEps);
    EXPECT_NEAR(TopEdgeY(flipped), 0.0f, kEps);
}

// The transform's local axes carry rotation and scale, so the pivot offset must
// travel along them rather than along world X/Y.
TEST(SpriteQuad, PivotOffsetFollowsTheTransformAxes)
{
    // 90 degrees about Z: local +X maps to world +Y, local +Y to world -X.
    float32 m[16];
    MakeTransform(m, 0.0f, 0.0f, 0.0f);
    m[0] = 0.0f;
    m[1] = 1.0f;
    m[4] = -1.0f;
    m[5] = 0.0f;

    const auto p = BuildSpriteQuad(m, 2.0f, 4.0f, 0.5f, 0.0f, false, false);

    // Feet pivot pushes the centre one half-height along local +Y = world -X.
    EXPECT_NEAR(p.Center[0], -2.0f, kEps);
    EXPECT_NEAR(p.Center[1], 0.0f, kEps);
}

TEST(SpriteQuad, ScaleInTheTransformScalesTheQuad)
{
    float32 m[16];
    MakeTransform(m, 0.0f, 0.0f, 0.0f);
    m[0] = 2.0f; // local X scaled 2x
    m[5] = 3.0f; // local Y scaled 3x

    const auto p = BuildSpriteQuad(m, 2.0f, 2.0f, 0.5f, 0.0f, false, false);

    EXPECT_NEAR(p.AxisX[0], 2.0f, kEps);
    EXPECT_NEAR(p.AxisY[1], 3.0f, kEps);
    EXPECT_NEAR(BottomEdgeY(p), 0.0f, kEps);
    EXPECT_NEAR(TopEdgeY(p), 6.0f, kEps);
}

TEST(SpriteQuadPixelGrid, RotatedCameraSnapsScreenAxesAndPreservesDepth)
{
    // A camera looking along world X: screen right is world Z, up is world Y.
    float32 view[16]{};
    view[8] = 1.0f;
    view[5] = 1.0f;
    view[2] = -1.0f;
    view[15] = 1.0f;
    float32 center[3] = {3.37f, 0.39f, 0.34f};
    const float32 axisX[3] = {0.0f, 0.0f, 0.5f};
    const float32 axisY[3] = {0.0f, 0.5f, 0.0f};
    SnapSpriteToPixelGrid(center, axisX, axisY, 0.25f, view);
    EXPECT_FLOAT_EQ(center[0], 3.37f);
    EXPECT_NEAR(center[1] - 0.5f, 0.0f, kEps);
    EXPECT_NEAR(center[2] - 0.5f, -0.25f, kEps);
}

TEST(SpriteQuadPixelGrid, DefaultCameraKeepsSpriteEdgesOnWholeTexels)
{
    float32 view[16];
    MakeTransform(view, 0, 0, 0);
    float32 center[3] = {0.34f, 0.39f, 2.0f};
    const float32 axisX[3] = {0.5f, 0.0f, 0.0f};
    const float32 axisY[3] = {0.0f, 0.5f, 0.0f};
    SnapSpriteToPixelGrid(center, axisX, axisY, 0.25f, view);
    EXPECT_NEAR(center[0] - 0.5f, -0.25f, kEps);
    EXPECT_NEAR(center[1] - 0.5f, 0.0f, kEps);
    EXPECT_FLOAT_EQ(center[2], 2.0f);
}
