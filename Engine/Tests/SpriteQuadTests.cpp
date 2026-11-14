#include "Engine/Rendering/2D/SpriteQuad.h"
#include "Engine/Rendering/SpritePixelGrid.h"

#include <gtest/gtest.h>

using namespace GameEngine;
using namespace GameEngine::Engine::Renderer;

namespace
{

// Identity basis with an exact world position.
void MakeTransform(Components::WorldTransform& m, float32 x, float32 y, float32 z)
{
    m = Components::WorldTransform{};
    m.Position = Mathematics::WorldPosition{x, y, z};
}

float32 BottomEdgeY(const SpriteQuadPlacement& p) { return p.Center.Y - p.AxisY[1]; }
float32 TopEdgeY(const SpriteQuadPlacement& p) { return p.Center.Y + p.AxisY[1]; }
float32 LeftEdgeX(const SpriteQuadPlacement& p) { return p.Center.X - p.AxisX[0]; }
float32 RightEdgeX(const SpriteQuadPlacement& p) { return p.Center.X + p.AxisX[0]; }

constexpr float32 kEps = 1.0e-5f;

} // namespace

TEST(SpriteQuad, CenterPivotPutsTheQuadCentreOnTheEntity)
{
    Components::WorldTransform m;
    MakeTransform(m, 3.0f, 7.0f, -2.0f);
    const auto p = BuildSpriteQuad(m, 2.0f, 3.0f, 0.5f, 0.5f, false, false);

    EXPECT_NEAR(p.Center.X, 3.0f, kEps);
    EXPECT_NEAR(p.Center.Y, 7.0f, kEps);
    EXPECT_NEAR(p.Center.Z, -2.0f, kEps);
    EXPECT_NEAR(p.AxisX[0], 1.0f, kEps);
    EXPECT_NEAR(p.AxisY[1], 1.5f, kEps);
}

// The headline case: pivot (0.5, 0) means "the entity position IS the feet".
TEST(SpriteQuad, FeetPivotPutsTheBottomEdgeOnTheEntity)
{
    Components::WorldTransform m;
    MakeTransform(m, 0.0f, 5.0f, 0.0f);
    const auto p = BuildSpriteQuad(m, 2.0f, 3.0f, 0.5f, 0.0f, false, false);

    EXPECT_NEAR(BottomEdgeY(p), 5.0f, kEps);
    EXPECT_NEAR(TopEdgeY(p), 8.0f, kEps);
    EXPECT_NEAR(p.Center.X, 0.0f, kEps);
}

// Regression for the "attack animation lifts the character off the ground" bug:
// a taller frame must grow UPWARD from the feet, never move them.
TEST(SpriteQuad, FeetPivotKeepsTheFeetStillWhenFrameHeightChanges)
{
    Components::WorldTransform m;
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
    Components::WorldTransform m;
    MakeTransform(m, 0.0f, 0.0f, 0.0f);

    const auto idle = BuildSpriteQuad(m, 2.0f, 2.0f, 0.5f, 0.5f, false, false);
    const auto attack = BuildSpriteQuad(m, 2.0f, 4.0f, 0.5f, 0.5f, false, false);

    EXPECT_NEAR(BottomEdgeY(idle), -1.0f, kEps);
    EXPECT_NEAR(BottomEdgeY(attack), -2.0f, kEps);
}

TEST(SpriteQuad, FlipXMirrorsAboutThePivotNotTheQuadCentre)
{
    Components::WorldTransform m;
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
    Components::WorldTransform m;
    MakeTransform(m, 2.0f, 0.0f, 0.0f);

    const auto normal = BuildSpriteQuad(m, 4.0f, 2.0f, 0.5f, 0.0f, false, false);
    const auto flipped = BuildSpriteQuad(m, 4.0f, 2.0f, 0.5f, 0.0f, true, false);

    EXPECT_NEAR(normal.Center.X, flipped.Center.X, kEps);
    EXPECT_NEAR(normal.Center.Y, flipped.Center.Y, kEps);
}

TEST(SpriteQuad, FlipYMirrorsAboutThePivot)
{
    Components::WorldTransform m;
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
    Components::WorldTransform m;
    MakeTransform(m, 0.0f, 0.0f, 0.0f);
    m.Basis[0].x = 0.0f;
    m.Basis[0].y = 1.0f;
    m.Basis[1].x = -1.0f;
    m.Basis[1].y = 0.0f;

    const auto p = BuildSpriteQuad(m, 2.0f, 4.0f, 0.5f, 0.0f, false, false);

    // Feet pivot pushes the centre one half-height along local +Y = world -X.
    EXPECT_NEAR(p.Center.X, -2.0f, kEps);
    EXPECT_NEAR(p.Center.Y, 0.0f, kEps);
}

TEST(SpriteQuad, ScaleInTheTransformScalesTheQuad)
{
    Components::WorldTransform m;
    MakeTransform(m, 0.0f, 0.0f, 0.0f);
    m.Basis[0].x = 2.0f; // local X scaled 2x
    m.Basis[1].y = 3.0f; // local Y scaled 3x

    const auto p = BuildSpriteQuad(m, 2.0f, 2.0f, 0.5f, 0.0f, false, false);

    EXPECT_NEAR(p.AxisX[0], 2.0f, kEps);
    EXPECT_NEAR(p.AxisY[1], 3.0f, kEps);
    EXPECT_NEAR(BottomEdgeY(p), 0.0f, kEps);
    EXPECT_NEAR(TopEdgeY(p), 6.0f, kEps);
}

TEST(SpriteQuadPixelGrid, RotatedCameraSnapsScreenAxesAndPreservesDepth)
{
    // A camera looking along world X: screen right is world Z, up is world Y.
    Mathematics::Matrix4x4 view;
    view[0][0] = view[2][2] = 0.0f;
    view[2][0] = 1.0f;
    view[0][2] = -1.0f;
    float32 center[3] = {3.37f, 0.39f, 0.34f};
    const float32 axisX[3] = {0.0f, 0.0f, 0.5f};
    const float32 axisY[3] = {0.0f, 0.5f, 0.0f};
    SnapSpriteToPixelGrid(center, axisX, axisY, 0.25f, view.Data(), Mathematics::WorldPosition{});
    EXPECT_FLOAT_EQ(center[0], 3.37f);
    EXPECT_NEAR(center[1] - 0.5f, 0.0f, kEps);
    EXPECT_NEAR(center[2] - 0.5f, -0.25f, kEps);
}

TEST(SpriteQuadPixelGrid, DefaultCameraKeepsSpriteEdgesOnWholeTexels)
{
    const Mathematics::Matrix4x4 view;
    float32 center[3] = {0.34f, 0.39f, 2.0f};
    const float32 axisX[3] = {0.5f, 0.0f, 0.0f};
    const float32 axisY[3] = {0.0f, 0.5f, 0.0f};
    SnapSpriteToPixelGrid(center, axisX, axisY, 0.25f, view.Data(), Mathematics::WorldPosition{});
    EXPECT_NEAR(center[0] - 0.5f, -0.25f, kEps);
    EXPECT_NEAR(center[1] - 0.5f, 0.0f, kEps);
    EXPECT_FLOAT_EQ(center[2], 2.0f);
}

TEST(SpriteQuad, SmallPivotOffsetSurvivesLargeWorldPosition)
{
    Components::WorldTransform pose;
    pose.Position = Mathematics::WorldPosition{1.0e8, -1.0e8, 1.0e8};
    const auto placed = BuildSpriteQuad(pose, 0.25f, 0.5f, 0.0f, 0.0f, false, false);
    EXPECT_DOUBLE_EQ(placed.Center.X - pose.Position.X, 0.125);
    EXPECT_DOUBLE_EQ(placed.Center.Y - pose.Position.Y, 0.25);
}

TEST(SpritePixelGrid, SectorRebasePreservesNonBinaryWorldTexelPhase)
{
    float32 center[]{0.125f, 0.0f, 0.0f};
    const float32 axisX[]{0.0f, 0.0f, 0.0f};
    const float32 axisY[]{0.0f, 0.0f, 0.0f};
    const Mathematics::Matrix4x4 view;
    constexpr float32 kStep = 0.03f;
    const Mathematics::WorldPosition origin{1024.0, 0.0, 0.0};
    SnapSpriteToPixelGrid(center, axisX, axisY, kStep, view.Data(), origin);
    const double expected = std::round(1024.125 / static_cast<double>(kStep)) * kStep - origin.X;
    EXPECT_NEAR(center[0], expected, 1e-6);
}
