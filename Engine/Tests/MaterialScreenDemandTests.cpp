// The demand of material mip streaming: the largest screen coverage of each material's instances.

#include "Engine/Rendering/MaterialMipStreaming/MaterialScreenDemand.h"

#include "Engine/Rendering/RenderOrigin.h"
#include "Mathematics/MatrixOps.h"
#include "Mathematics/WorldPosition.h"
#include "Rendering/Camera/CameraTypes.h"
#include "Rendering/Core/GPUScene.h"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

using namespace GameEngine;
using GameEngine::Engine::Renderer::MaterialScreenDemand;
using GameEngine::Rendering::GPUInstance;

namespace
{

// Reads every row in one update and completes the sweep in it.
constexpr MaterialScreenDemand::Pace kWholeSweep{1u << 20, 1u};

GPUInstance MakeRow(uint32 materialIndex, const Mathematics::Vector3& center, float32 radius)
{
    GPUInstance row{};
    row.meshIndex = 0;
    row.materialIndex = materialIndex;
    row.boundingCenter = center;
    row.boundingRadius = radius;
    return row;
}

GPUInstance MakeFreedRow()
{
    GPUInstance row{};
    row.meshIndex = Rendering::kTombstoneIndex;
    row.materialIndex = Rendering::kTombstoneIndex;
    return row;
}

MaterialScreenDemand::View MakeView(float32 projScaleY)
{
    MaterialScreenDemand::View view;
    view.ProjScaleY = projScaleY;
    return view;
}

// A camera at `eye` that looks along +Z, built as an editor builds it: the double eye through SetCameraEye.
Rendering::CameraData CameraAt(const Mathematics::WorldPosition& eye, const Mathematics::Matrix4x4& proj)
{
    const Mathematics::Vector3 eyeLocal = eye - Mathematics::WorldPosition{};
    Rendering::CameraData camera{};
    camera.view = Mathematics::MakeLookAtLH(eyeLocal, eyeLocal + Mathematics::Vector3{0.0f, 0.0f, 1.0f},
                                            Mathematics::Vector3{0.0f, 1.0f, 0.0f});
    camera.proj = proj;
    camera.viewProj = camera.proj * camera.view;
    Engine::Renderer::SetCameraEye(camera, eye);
    return camera;
}

} // namespace

TEST(MaterialScreenDemand, CoverageIsRadiusTimesProjectionScaleOverSurfaceDistance)
{
    const std::vector rows{MakeRow(2, {0.0f, 0.0f, 102.0f}, 2.0f)};

    MaterialScreenDemand demand;
    ASSERT_TRUE(demand.Advance(rows, MakeView(1.5f), kWholeSweep));

    const auto coverage = demand.Coverage();
    ASSERT_EQ(coverage.size(), 3u);
    EXPECT_FLOAT_EQ(coverage[2], 2.0f * 1.5f / 100.0f);
    EXPECT_EQ(coverage[0], 0.0f);
    EXPECT_EQ(coverage[1], 0.0f);
}

TEST(MaterialScreenDemand, AMaterialTakesTheLargestCoverageOfItsRows)
{
    const std::vector rows{MakeRow(0, {0.0f, 0.0f, 201.0f}, 1.0f), MakeRow(0, {0.0f, 0.0f, 11.0f}, 1.0f),
                           MakeRow(0, {0.0f, 0.0f, 51.0f}, 1.0f)};

    MaterialScreenDemand demand;
    ASSERT_TRUE(demand.Advance(rows, MakeView(1.0f), kWholeSweep));

    ASSERT_EQ(demand.Coverage().size(), 1u);
    EXPECT_FLOAT_EQ(demand.Coverage()[0], 1.0f / 10.0f);
}

TEST(MaterialScreenDemand, ACameraInsideTheSphereGivesTheHighestCoverage)
{
    const std::vector rows{MakeRow(0, {0.0f, 0.0f, 10.0f}, 500.0f), MakeRow(1, {0.0f, 0.0f, 3.0f}, 1.0f)};

    MaterialScreenDemand demand;
    ASSERT_TRUE(demand.Advance(rows, MakeView(1.0f), kWholeSweep));

    EXPECT_GT(demand.Coverage()[0], demand.Coverage()[1]);
    EXPECT_TRUE(std::isfinite(demand.Coverage()[0]));
}

TEST(MaterialScreenDemand, AFreedRowGivesNoDemand)
{
    const std::vector rows{MakeFreedRow(), MakeRow(1, {0.0f, 0.0f, 11.0f}, 1.0f), MakeFreedRow()};

    MaterialScreenDemand demand;
    ASSERT_TRUE(demand.Advance(rows, MakeView(1.0f), kWholeSweep));

    ASSERT_EQ(demand.Coverage().size(), 2u);
    EXPECT_EQ(demand.Coverage()[0], 0.0f);
    EXPECT_FLOAT_EQ(demand.Coverage()[1], 0.1f);
}

TEST(MaterialScreenDemand, ASweepPublishesOnlyWhenItHasReadEveryRow)
{
    std::vector<GPUInstance> rows;
    for (uint32 material = 0; material < 5; ++material)
        rows.push_back(MakeRow(material, {0.0f, 0.0f, 11.0f}, 1.0f));

    MaterialScreenDemand demand;
    EXPECT_FALSE(demand.Advance(rows, MakeView(1.0f), MaterialScreenDemand::Pace{2u, 1u}));
    EXPECT_TRUE(demand.Coverage().empty());
    EXPECT_FALSE(demand.Advance(rows, MakeView(1.0f), MaterialScreenDemand::Pace{2u, 1u}));
    EXPECT_TRUE(demand.Coverage().empty());
    EXPECT_TRUE(demand.Advance(rows, MakeView(1.0f), MaterialScreenDemand::Pace{2u, 1u}));

    ASSERT_EQ(demand.Coverage().size(), 5u);
    for (const float32 coverage : demand.Coverage())
        EXPECT_FLOAT_EQ(coverage, 0.1f);
}

TEST(MaterialScreenDemand, TheNextSweepDropsAMaterialWithNoRowLeft)
{
    std::vector rows{MakeRow(0, {0.0f, 0.0f, 11.0f}, 1.0f), MakeRow(1, {0.0f, 0.0f, 11.0f}, 1.0f)};

    MaterialScreenDemand demand;
    ASSERT_TRUE(demand.Advance(rows, MakeView(1.0f), kWholeSweep));
    ASSERT_FLOAT_EQ(demand.Coverage()[1], 0.1f);

    rows[1] = MakeFreedRow();
    ASSERT_TRUE(demand.Advance(rows, MakeView(1.0f), kWholeSweep));

    EXPECT_FLOAT_EQ(demand.Coverage()[0], 0.1f);
    EXPECT_EQ(demand.Coverage()[1], 0.0f);
}

TEST(MaterialScreenDemand, ASweepEndsWhenTheSceneTrimsRowsPastItsCursor)
{
    std::vector<GPUInstance> rows(6, MakeRow(0, {0.0f, 0.0f, 11.0f}, 1.0f));

    MaterialScreenDemand demand;
    ASSERT_FALSE(demand.Advance(rows, MakeView(1.0f), MaterialScreenDemand::Pace{4u, 1u}));
    rows.resize(2);

    EXPECT_TRUE(demand.Advance(rows, MakeView(1.0f), MaterialScreenDemand::Pace{4u, 1u}));
    EXPECT_FLOAT_EQ(demand.Coverage()[0], 0.1f);
}

TEST(MaterialScreenDemand, DistanceCrossesSectors)
{
    // The row is in the sector next to the view's origin sector, 10 m past the shared face.
    constexpr float32 kSector = static_cast<float32>(Mathematics::kWorldSectorSize);
    GPUInstance row = MakeRow(0, {10.0f, 0.0f, 0.0f}, 1.0f);
    Rendering::PackSector(4, 0, 0, row.sectorPacked[0], row.sectorPacked[1]);
    const std::vector rows{row};

    MaterialScreenDemand::View view = MakeView(1.0f);
    view.OriginSector = {3, 0, 0};
    view.Eye = {kSector - 10.0f, 0.0f, 0.0f};

    MaterialScreenDemand demand;
    ASSERT_TRUE(demand.Advance(rows, view, kWholeSweep));

    EXPECT_FLOAT_EQ(demand.Coverage()[0], 1.0f / 19.0f);
}

TEST(MaterialScreenDemand, ResetDropsTheTableAndTheSweepInProgress)
{
    const std::vector<GPUInstance> rows(4, MakeRow(0, {0.0f, 0.0f, 11.0f}, 1.0f));

    MaterialScreenDemand demand;
    ASSERT_TRUE(demand.Advance(rows, MakeView(1.0f), kWholeSweep));
    ASSERT_FALSE(demand.Advance(rows, MakeView(1.0f), MaterialScreenDemand::Pace{2u, 1u}));
    demand.Reset();

    EXPECT_TRUE(demand.Coverage().empty());
    EXPECT_FALSE(demand.Advance(rows, MakeView(1.0f), MaterialScreenDemand::Pace{3u, 1u}));
    EXPECT_TRUE(demand.Advance(rows, MakeView(1.0f), MaterialScreenDemand::Pace{3u, 1u}));
}

TEST(MaterialScreenDemand, AStillViewOfAStillScenePublishesOneTime)
{
    const std::vector rows{MakeRow(0, {0.0f, 0.0f, 11.0f}, 1.0f)};

    MaterialScreenDemand demand;
    EXPECT_TRUE(demand.Advance(rows, MakeView(1.0f), kWholeSweep));
    EXPECT_FALSE(demand.Advance(rows, MakeView(1.0f), kWholeSweep));
    EXPECT_FALSE(demand.Advance(rows, MakeView(1.0f), kWholeSweep));
    EXPECT_FLOAT_EQ(demand.Coverage()[0], 0.1f);

    MaterialScreenDemand::View moved = MakeView(1.0f);
    moved.Eye = {0.0f, 0.0f, 5.0f};
    EXPECT_TRUE(demand.Advance(rows, moved, kWholeSweep));
    EXPECT_FLOAT_EQ(demand.Coverage()[0], 0.2f);
}

TEST(MaterialScreenDemand, ASweepOfASmallSceneTakesItsMinimumUpdates)
{
    const std::vector rows{MakeRow(0, {0.0f, 0.0f, 11.0f}, 1.0f)};
    constexpr MaterialScreenDemand::Pace kThreeUpdates{16u, 3u};

    MaterialScreenDemand demand;
    EXPECT_FALSE(demand.Advance(rows, MakeView(1.0f), kThreeUpdates));
    EXPECT_FALSE(demand.Advance(rows, MakeView(1.0f), kThreeUpdates));
    EXPECT_TRUE(demand.Coverage().empty());
    EXPECT_TRUE(demand.Advance(rows, MakeView(1.0f), kThreeUpdates));
    EXPECT_FLOAT_EQ(demand.Coverage()[0], 0.1f);
}

TEST(MaterialScreenDemand, AnEmptySceneNeverPublishes)
{
    MaterialScreenDemand demand;
    for (int update = 0; update < 4; ++update)
        EXPECT_FALSE(demand.Advance({}, MakeView(1.0f), kWholeSweep));
}

TEST(MaterialScreenDemand, OrthographicCoverageDoesNotChangeWithDistance)
{
    const std::vector rows{MakeRow(0, {0.0f, 0.0f, 11.0f}, 2.0f), MakeRow(1, {0.0f, 0.0f, 1001.0f}, 2.0f)};
    MaterialScreenDemand::View view = MakeView(0.25f);
    view.Orthographic = true;

    MaterialScreenDemand demand;
    ASSERT_TRUE(demand.Advance(rows, view, kWholeSweep));

    EXPECT_FLOAT_EQ(demand.Coverage()[0], 0.5f);
    EXPECT_FLOAT_EQ(demand.Coverage()[1], 0.5f);
}

TEST(MaterialScreenDemand, TheViewOfACameraCarriesItsEyeAcrossSectors)
{
    // The eye is 5 sectors and 40 m from the world origin. The row is 100 m in front of it.
    constexpr double kSector = Mathematics::kWorldSectorSize;
    const Rendering::CameraData camera =
        CameraAt(Mathematics::WorldPosition{5.0 * kSector + 40.0, 0.0, 0.0},
                 Mathematics::MakePerspectiveLH_ZO_ReverseZ(1.0472f, 1.0f, 0.1f, 1000.0f));
    GPUInstance row = MakeRow(0, {40.0f, 0.0f, 100.0f}, 1.0f);
    Rendering::PackSector(5, 0, 0, row.sectorPacked[0], row.sectorPacked[1]);
    const std::vector rows{row};

    const auto view = MaterialScreenDemand::ViewOf(camera);
    ASSERT_TRUE(view.has_value());
    EXPECT_FALSE(view->Orthographic);
    MaterialScreenDemand demand;
    ASSERT_TRUE(demand.Advance(rows, *view, kWholeSweep));

    const float32 expected = std::abs(camera.proj[1][1]) / 99.0f;
    EXPECT_NEAR(demand.Coverage()[0], expected, expected * 1.0e-3f);
}

TEST(MaterialScreenDemand, TheViewOfAnOrthographicCameraIsOrthographic)
{
    const Rendering::CameraData camera =
        CameraAt(Mathematics::WorldPosition{0.0, 10.0, 0.0},
                 Mathematics::MakeOrthographicLH_ZO_ReverseZ(-8.0f, 8.0f, -4.0f, 4.0f, 0.1f, 100.0f));

    const auto view = MaterialScreenDemand::ViewOf(camera);

    ASSERT_TRUE(view.has_value());
    EXPECT_TRUE(view->Orthographic);
    EXPECT_FLOAT_EQ(view->ProjScaleY, 0.25f);
}

TEST(MaterialScreenDemand, ACameraWithNoEyeHasNoView)
{
    EXPECT_FALSE(MaterialScreenDemand::ViewOf(Rendering::CameraData{}).has_value());
}
