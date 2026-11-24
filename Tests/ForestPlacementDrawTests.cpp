#include "PackedPlacementTestArchive.h"

#include "Assets/AssetManager.h"
#include "Assets/ModelAsset.h"

#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ForestImpostorDraw.h"
#include "ForestNearMeshCandidates.h"
#include "ForestPlacementGpuData.h"
#include "ForestPlacementSession.h"
#include "ForestPlacementRuntime.h"
#include "ForestStandDrawProducer.h"
#include "ForestStandDraws.h"
#include "../Engine/Source/Engine/Rendering/IdentityMeshDrawConstants.h"
#include "ForestPlacementFeature.h"
#include "EngineLogCapture.h"
#include "Engine/Rendering/Impostors/ImpostorStage.h"
#include "Engine/Rendering/MaterialSystem.h"
#include "Engine/Rendering/RenderOrigin.h"
#include "Engine/Rendering/RenderServices.h"
#include "Mathematics/MatrixOps.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/Pipeline/PipelineTypes.h"
#include "Rendering/Core/GPUScene.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Materials/MaterialDocument.h"
#include "Tests/ScopedEnvVar.h"
#include "StagedTestPaths.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace GameEngine::PageStreaming
{
namespace
{

class ForestPlacementDrawTest : public PackedPlacementArchiveTest
{
};

TEST_F(ForestPlacementDrawTest, NearMeshTransformsRetainPackedSectorYawAndScale)
{
    using namespace Engine::Renderer;
    ForestNearMeshCandidate tree{{}, 0, {512'000'000.25, 8.5, -511'999'999.875}, 1.5707963f, 1.25f, 0};
    const auto row = PackForestMeshTransform(tree);
    ASSERT_TRUE(row);
    Mathematics::Vector3Int sector;
    Rendering::UnpackSector(row->SectorPacked[0], row->SectorPacked[1], sector.X, sector.Y, sector.Z);
    const auto world = Mathematics::WorldPosition::FromSector(
        sector, {row->LocalAndScale.x, row->LocalAndScale.y, row->LocalAndScale.z});
    EXPECT_DOUBLE_EQ(world.X, tree.Root.X);
    EXPECT_DOUBLE_EQ(world.Y, tree.Root.Y);
    EXPECT_DOUBLE_EQ(world.Z, tree.Root.Z);
    EXPECT_FLOAT_EQ(row->LocalAndScale.w, 1.25f);
    EXPECT_FLOAT_EQ(row->YawSin, 1.0f);
    EXPECT_NEAR(row->YawCos, 0.0f, 1e-6f);

    tree.Root.X = (static_cast<double>(Rendering::kSectorAxisMax) + 1.0) * Mathematics::kWorldSectorSize;
    EXPECT_FALSE(PackForestMeshTransform(tree));
}

TEST_F(ForestPlacementDrawTest, CardEntryRespectsTheCoarsestLevelOfEveryPart)
{
    using namespace Engine::Renderer;
    Rendering::GPUMesh bark{};
    bark.lodCount = 2;
    bark.lodThreshold[0] = 0.2f;
    Rendering::GPUMesh leaves{};
    leaves.lodCount = 3;
    leaves.lodFlags = Rendering::kGPUMeshLodTightClassBit | 1u;
    leaves.lodThreshold[0] = 0.4f;
    leaves.lodThreshold[1] = 0.1f;
    leaves.lodSseScaleCeil = 0.12f;
    leaves.lodCoverageScale = 0.5f;

    const float margin = 1.0f + ImpostorStage::kSwitchHysteresis;
    EXPECT_FLOAT_EQ(ForestPartCardEntryCoverage(bark, 0.4f, 0.2f), 0.2f / margin);
    // The first leaf threshold becomes .048. The authored .1 threshold must
    // descend below it to .024. The model-sphere threshold is .048.
    EXPECT_FLOAT_EQ(ForestPartCardEntryCoverage(leaves, 0.4f, 0.2f), 0.048f / margin);
    EXPECT_FLOAT_EQ(ForestPartCardEntryCoverage(leaves, 0.4f, 0.06f), 0.024f / margin);
    EXPECT_EQ(SelectForestMeshLod(leaves, 0.12f, 0.4f, 0.2f, 0xFFFFFFFFu), 0u);
    EXPECT_EQ(SelectForestMeshLod(leaves, 0.07f, 0.4f, 0.2f, 0xFFFFFFFFu), 1u);
    EXPECT_EQ(SelectForestMeshLod(leaves, 0.03f, 0.4f, 0.2f, 0xFFFFFFFFu), 2u);
    EXPECT_EQ(SelectForestMeshLod(leaves, 0.03f, 0.4f, 0.2f, 1u), 1u);
    leaves.lodThreshold[0] = 0.0f;
    EXPECT_EQ(ForestPartCardEntryCoverage(leaves, 0.4f, 0.2f), 0.0f);
    leaves.lodCount = 1;
    EXPECT_GT(ForestPartCardEntryCoverage(leaves, 0.4f, 0.2f), 1.0f);
}

TEST_F(ForestPlacementDrawTest, DeformationWidensTheNearMeshFrustumWithoutMovingItsLodSwitch)
{
    using namespace Engine::Renderer;
    const auto file = m_Scratch.Path() / "wind-frustum.geforest";
    WriteStore(file);
    std::string error;
    auto runtime = ForestPlacementRuntime::Open(file, {.DetailSlots = 1, .ReadsPerFrame = 1, .KeepCpuCopy = true}, error);
    ASSERT_TRUE(runtime) << error;
    const ForestPlacementView view{{512'000'000.0, 8.0, 512'000'001.0}, {}, 100.0f};
    ForestPlacementFrame frame;
    ASSERT_TRUE(runtime->Prepare(view, 1, 0, {}, frame));
    ASSERT_EQ(frame.Reads.size(), 1u);
    const auto ticket = frame.Reads.front();
    ASSERT_EQ(ticket.Page, 0u);
    ASSERT_TRUE(runtime->CompleteRead(ticket, kFirstPage));
    ASSERT_TRUE(runtime->CompleteUpload(ticket, ForestUploadResult::Published));

    ForestNearMeshView near{view, 2.0f, 0.0f, 0.0f};
    const Mathematics::Vector4 bounds{0, 1, 0, 2};
    std::vector<ForestNearMeshCandidate> candidates;
    ASSERT_TRUE(runtime->Prepare(view, 2, 1, {}, frame));
    CollectForestNearMeshCandidates(*runtime, frame.DetailedDraws, near, bounds, 1, candidates);
    ASSERT_EQ(candidates.size(), 1u);
    const float restCoverage = candidates.front().Coverage;

    // Outside the rest sphere even with the shared 1.5x draw guard band.
    near.PlacementView.EyeRelativePlanes[0] = {1, 0, 0, -3.7f};
    ASSERT_TRUE(runtime->Prepare(near.PlacementView, 3, 2, {}, frame));
    ASSERT_EQ(frame.DetailedDraws.size(), 1u);
    CollectForestNearMeshCandidates(*runtime, frame.DetailedDraws, near, bounds, 1, candidates);
    EXPECT_TRUE(candidates.empty());
    near.DeformationMargin = 0.8f;
    CollectForestNearMeshCandidates(*runtime, frame.DetailedDraws, near, bounds, 1, candidates);
    ASSERT_EQ(candidates.size(), 1u);
    EXPECT_FLOAT_EQ(candidates.front().Coverage, restCoverage);
    EXPECT_EQ(candidates.front().Ticket, ticket);
}

TEST_F(ForestPlacementDrawTest, NearMeshHysteresisKeepsOnlyTheSamePlacementGeneration)
{
    using namespace Engine::Renderer;
    const auto file = m_Scratch.Path() / "hysteresis.geforest";
    WriteStore(file);
    std::string error;
    auto runtime = ForestPlacementRuntime::Open(file, {.DetailSlots = 1, .ReadsPerFrame = 1, .KeepCpuCopy = true}, error);
    ASSERT_TRUE(runtime) << error;
    const ForestPlacementView view{{512'000'000.0, 8.0, 512'000'001.0}, {}, 100.0f};
    ForestPlacementFrame frame;
    ASSERT_TRUE(runtime->Prepare(view, 1, 0, {}, frame));
    ASSERT_EQ(frame.Reads.size(), 1u);
    const auto ticket = frame.Reads.front();
    ASSERT_EQ(ticket.Page, 0u);
    ASSERT_TRUE(runtime->CompleteRead(ticket, kFirstPage));
    ASSERT_TRUE(runtime->CompleteUpload(ticket, ForestUploadResult::Published));
    ASSERT_TRUE(runtime->Prepare(view, 2, 1, {}, frame));

    const Mathematics::Vector4 bounds{0, 1, 0, 2};
    ForestNearMeshView near{view, 2.0f, 0.0f, 0.0f};
    std::vector<ForestNearMeshCandidate> candidates;
    CollectForestNearMeshCandidates(*runtime, frame.DetailedDraws, near, bounds, 1, candidates);
    ASSERT_EQ(candidates.size(), 1u);
    const float coverage = candidates.front().Coverage;
    const ForestMeshOwnedPlacement current{ticket, candidates.front().LocalPlacement};

    near.ImpostorCoverage = coverage * 0.95f;
    CollectForestNearMeshCandidates(*runtime, frame.DetailedDraws, near, bounds, 1, candidates);
    EXPECT_TRUE(candidates.empty());
    CollectForestNearMeshCandidates(*runtime, frame.DetailedDraws, near, bounds, 1, candidates, std::span(&current, 1));
    ASSERT_EQ(candidates.size(), 1u);

    auto retired = current;
    ++retired.Ticket.Generation;
    CollectForestNearMeshCandidates(*runtime, frame.DetailedDraws, near, bounds, 1, candidates, std::span(&retired, 1));
    EXPECT_TRUE(candidates.empty());

    near.ImpostorCoverage = coverage * 1.01f;
    CollectForestNearMeshCandidates(*runtime, frame.DetailedDraws, near, bounds, 1, candidates, std::span(&current, 1));
    EXPECT_TRUE(candidates.empty());
    near.ImpostorCoverage = coverage * 0.75f;
    CollectForestNearMeshCandidates(*runtime, frame.DetailedDraws, near, bounds, 1, candidates);
    EXPECT_EQ(candidates.size(), 1u);
}

TEST_F(ForestPlacementDrawTest, NearMeshesUseOnlyVerifiedAndPublishedPlacementTransforms)
{
    using namespace Engine::Renderer;
    const auto file = m_Scratch.Path() / "near-mesh.geforest";
    WriteStore(file);
    std::string error;
    auto runtime = ForestPlacementRuntime::Open(file, {.DetailSlots = 2, .ReadsPerFrame = 2, .KeepCpuCopy = true}, error);
    ASSERT_TRUE(runtime) << error;
    const ForestPlacementView view{{512'000'000.0, 8.0, 512'000'001.0}, {}, 100.0f};
    ForestPlacementFrame frame;
    ASSERT_TRUE(runtime->Prepare(view, 1, 0, {}, frame));
    ASSERT_EQ(frame.Reads.size(), 2u);
    const auto first = frame.Reads[0];
    const auto second = frame.Reads[1];
    EXPECT_EQ(first.Page, 0u);
    EXPECT_EQ(second.Page, 1u);
    EXPECT_TRUE(runtime->ReadyPlacements(first).empty());

    auto returned = kFirstPage;
    ASSERT_TRUE(runtime->CompleteRead(first, returned));
    returned.fill(0); // The I/O span does not outlive the read callback.
    ASSERT_TRUE(runtime->CompleteRead(second, kSecondPage));
    EXPECT_TRUE(runtime->ReadyPlacements(first).empty());
    ASSERT_TRUE(runtime->CompleteUpload(first, ForestUploadResult::Published));
    ASSERT_TRUE(runtime->CompleteUpload(second, ForestUploadResult::Published));

    ASSERT_TRUE(runtime->Prepare(view, 2, 1, {}, frame));
    std::vector<ForestNearMeshCandidate> candidates;
    const ForestNearMeshView near{view, 2.0f, 0.0f, 0.01f};
    CollectForestNearMeshCandidates(*runtime, frame.DetailedDraws, near, {0, 1, 0, 2}, 1, candidates);
    ASSERT_EQ(candidates.size(), 1u);
    const auto& placement = candidates.front();
    EXPECT_EQ(placement.Ticket, first);
    EXPECT_EQ(placement.LocalPlacement, 0u);
    EXPECT_DOUBLE_EQ(placement.Root.X, 512'000'000.12);
    EXPECT_DOUBLE_EQ(placement.Root.Y, 8.0);
    EXPECT_DOUBLE_EQ(placement.Root.Z, 512'000'001.99);
    EXPECT_FLOAT_EQ(placement.Scale, 0.75f + 0.5f * 128.0f / 255.0f);
    const auto secondRecords = runtime->ReadyPlacements(second);
    ASSERT_EQ(secondRecords.size(), 2u);
    EXPECT_EQ(secondRecords[0].Height, 23u);
    std::vector<ForestNearMeshCandidate> all;
    CollectForestNearMeshCandidates(*runtime, frame.DetailedDraws, near, {0, 1, 0, 2}, 3, all);
    const auto raised = std::ranges::find_if(all, [&second](const ForestNearMeshCandidate& tree)
                                             { return tree.Ticket == second && tree.LocalPlacement == 0; });
    ASSERT_NE(raised, all.end());
    EXPECT_DOUBLE_EQ(raised->Root.Y, 8.23);
    EXPECT_NEAR(secondRecords[0].YawRadians(), 255.0 * 6.283185307179586 / 256.0, 1e-12);

    auto turned = near;
    turned.PlacementView.EyeRelativePlanes[0] = {1, 0, 0, -40};
    ASSERT_TRUE(runtime->Prepare(turned.PlacementView, 3, 2, {}, frame));
    EXPECT_TRUE(runtime->ReadyPlacements(first).empty());
    CollectForestNearMeshCandidates(*runtime, frame.DetailedDraws, turned, {0, 1, 0, 2}, 4, candidates);
    ASSERT_EQ(candidates.size(), 2u);
    EXPECT_EQ(candidates.front().Ticket, second);

    runtime->ResetResidency();
    EXPECT_TRUE(runtime->ReadyPlacements(second).empty());
    CollectForestNearMeshCandidates(*runtime, frame.DetailedDraws, turned, {0, 1, 0, 2}, 4, candidates);
    EXPECT_TRUE(candidates.empty());
}

// Opens a one-page, one-tree archive and makes the page resident for `view`.
std::shared_ptr<Engine::Renderer::ForestPlacementRuntime> OpenResidentTree(const std::filesystem::path& file,
    const PlacementPage& page, const PackedPlacement& tree, float boundRadius,
    const Engine::Renderer::ForestPlacementView& view, Engine::Renderer::ForestPlacementFrame& frame)
{
    using namespace Engine::Renderer;
    EXPECT_EQ(WritePackedPlacementStore(file, boundRadius, std::span(&page, 1u), std::span(&tree, 1u)), "");
    std::string error;
    auto runtime = ForestPlacementRuntime::Open(file, {.DetailSlots = 1, .ReadsPerFrame = 1, .KeepCpuCopy = true}, error);
    EXPECT_TRUE(runtime) << error;
    if (!runtime || !runtime->Prepare(view, 1, 0, {}, frame) || frame.Reads.size() != 1u)
        return nullptr;
    const auto ticket = frame.Reads.front();
    std::array<uint8, sizeof(PackedPlacement)> bytes{};
    std::memcpy(bytes.data(), &tree, sizeof(tree));
    if (!runtime->CompleteRead(ticket, bytes) || !runtime->CompleteUpload(ticket, ForestUploadResult::Published) ||
        !runtime->Prepare(view, 2, 1, {}, frame))
        return nullptr;
    return runtime;
}

TEST_F(ForestPlacementDrawTest, NearMeshDistanceTurnsTheModelCenterWithThePlacementYaw)
{
    using namespace Engine::Renderer;
    // A quarter turn moves the model center from +X to -Z: (x cos + z sin, y, -x sin + z cos).
    const PackedPlacement tree{1000, 0, 0, 64, 0};
    const PlacementPage page{.Origin = {0.0, 0.0, 0.0}, .LocalMin = {-5, -5, -5}, .LocalMax = {15, 5, 5},
                             .First = 0, .Count = 1};
    const ForestPlacementView view{{0.0, 0.0, 5.0}, {}, 100.0f};
    ForestPlacementFrame frame;
    const auto runtime = OpenResidentTree(m_Scratch.Path() / "yaw.geforest", page, tree, 4.0f, view, frame);
    ASSERT_TRUE(runtime);

    std::vector<ForestNearMeshCandidate> candidates;
    CollectForestNearMeshCandidates(*runtime, frame.DetailedDraws, {view, 2.0f, 0.0f, 0.0f}, {2, 1, 0, 2}, 1, candidates);
    ASSERT_EQ(candidates.size(), 1u);
    // Root (10, 0, 0), center offset (0, 1, -2) at scale 0.75, eye (0, 0, 5).
    EXPECT_NEAR(candidates.front().DistanceSquared, 10.0 * 10.0 + 0.75 * 0.75 + 6.5 * 6.5, 1e-4);
}

TEST_F(ForestPlacementDrawTest, NearMeshBroadphaseKeepsAPageWhoseLargestTreeStillCovers)
{
    using namespace Engine::Renderer;
    // A tree at the largest scale, 39 m from the eye, in a page that starts 38 m from the eye.
    const PackedPlacement tree{2000, 0, 0, 0, 255};
    const PlacementPage page{.Origin = {0.0, 0.0, 0.0}, .LocalMin = {19, -1, -1}, .LocalMax = {21, 1, 1},
                             .First = 0, .Count = 1};
    const ForestPlacementView view{{-19.0, 0.0, 0.0}, {}, 100.0f};
    ForestPlacementFrame frame;
    const auto runtime = OpenResidentTree(m_Scratch.Path() / "broadphase.geforest", page, tree, 0.5f, view, frame);
    ASSERT_TRUE(runtime);

    // The tree covers 1.25 * 1 * 2 / 39 of the view. A sphere at unit scale reaches that coverage only
    // within 2 / coverage = 31.5 m, short of the page's 38 m, so only the largest scale keeps the page.
    // The tree already draws its mesh, so the entry hysteresis does not apply.
    constexpr float kTreeCoverage = 1.25f * 2.0f / 39.0f;
    ASSERT_EQ(frame.DetailedDraws.size(), 1u);
    const ForestMeshOwnedPlacement owner{frame.DetailedDraws.front().Ticket, 0};
    std::vector<ForestNearMeshCandidate> candidates;
    CollectForestNearMeshCandidates(*runtime, frame.DetailedDraws, {view, 2.0f, 0.0f, kTreeCoverage * 0.99f}, {0, 0, 0, 1}, 1,
                                    candidates, std::span(&owner, 1u));
    EXPECT_EQ(candidates.size(), 1u);
}

TEST_F(ForestPlacementDrawTest, OrthographicCastersTakeMeshesOnlyAboveTheAtlasFootprint)
{
    using namespace Engine::Renderer;
    const PackedPlacement tree{2000, 0, 0, 0, 255};
    const PlacementPage page{.Origin = {0.0, 0.0, 0.0}, .LocalMin = {19, -1, -1}, .LocalMax = {21, 1, 1},
                             .First = 0, .Count = 1};
    const ForestPlacementView view{{-19.0, 0.0, 0.0}, {}, 100.0f};
    ForestPlacementFrame frame;
    const auto runtime = OpenResidentTree(m_Scratch.Path() / "ortho-caster.geforest", page, tree, 1.25f, view, frame);
    ASSERT_TRUE(runtime);

    const Mathematics::Vector4 sphere{0, 0, 0, 1};
    ForestNearMeshView cascade{view, 1.0f / 20.0f, 0.0f, 128.0f / 2048.0f, 0.0f, true};
    std::vector<ForestNearMeshCandidate> candidates;
    // At 20 m half extent the tree is exactly 128 texels across. Keep the card.
    CollectForestNearMeshCandidates(*runtime, frame.DetailedDraws, cascade, sphere, 1, candidates);
    EXPECT_TRUE(candidates.empty());

    // The same tree in a tighter cascade takes a mesh even far from the camera.
    cascade.ProjectionScaleY = 1.0f / 16.0f;
    CollectForestNearMeshCandidates(*runtime, frame.DetailedDraws, cascade, sphere, 1, candidates);
    ASSERT_EQ(candidates.size(), 1u);
    EXPECT_FLOAT_EQ(candidates.front().Coverage, 1.25f / 16.0f);
}

TEST_F(ForestPlacementDrawTest, ACardPastTheBoundRadiusRefusesItsSessionOnce)
{
    using namespace Engine::Renderer;
    const auto file = m_Scratch.Path() / "refused.geforest";
    WriteStore(file);
    std::string error;
    auto runtime = ForestPlacementRuntime::Open(file, {.DetailSlots = 1, .ReadsPerFrame = 1}, error);
    ASSERT_TRUE(runtime) << error;
    RenderServices services;
    AssetManager assets;
    auto& feature = services.EnsureFeature<ForestPlacementFeature>();
    constexpr uint64 kWorld = 7;
    const GUID model = GUID::Generate();
    const auto session = feature.Attach(runtime, assets, kWorld, model, 100.0f);
    ASSERT_TRUE(session);

    // The card corner reaches 10 + 4 * sqrt(2) m from the root, past the archive's 12 m.
    ForestImpostorCard card{};
    card.Bounds = {0.0f, 10.0f, 0.0f, 4.0f};
    std::vector<std::string> lines;
    {
        TestLog::ScopedEngineLogCapture capture(&lines);
        feature.PublishCard(kWorld, model, card);
        feature.PublishCard(kWorld, model, card);
        Logger::Log::Flush();
    }
    EXPECT_EQ(std::ranges::count_if(lines, [](const std::string& line)
              { return line.find("cooked with a bound radius of 12.00 m") != std::string::npos; }), 1);
    EXPECT_FALSE(feature.WantsCard(kWorld, model));
}

TEST_F(ForestPlacementDrawTest, ClearCardsForgetsOnlyItsWorldsCardsUntilTheNextPublish)
{
    using namespace Engine::Renderer;
    const auto file = m_Scratch.Path() / "cards.geforest";
    WriteStore(file);
    std::string error;
    auto runtime = ForestPlacementRuntime::Open(file, {.DetailSlots = 1, .ReadsPerFrame = 1}, error);
    ASSERT_TRUE(runtime) << error;
    RenderServices services;
    AssetManager assets;
    auto& feature = services.EnsureFeature<ForestPlacementFeature>();
    const GUID model = GUID::Generate();
    const auto cleared = feature.Attach(runtime, assets, 7, model, 100.0f);
    const auto other = feature.Attach(runtime, assets, 8, model, 100.0f);
    ASSERT_TRUE(cleared && other);
    cleared->WantsCard = true;
    other->WantsCard = true;
    ForestImpostorCard card{};
    card.Bounds = {0.0f, 5.0f, 0.0f, 2.0f};
    feature.PublishCard(7, model, card);
    feature.PublishCard(8, model, card);
    ASSERT_TRUE(cleared->Card && other->Card);

    feature.ClearCards(7);
    EXPECT_FALSE(cleared->Card);
    EXPECT_TRUE(other->Card);
    EXPECT_TRUE(feature.WantsCard(7, model));
    feature.PublishCard(7, model, card);
    ASSERT_TRUE(cleared->Card);
    EXPECT_EQ(cleared->Card->Bounds.y, 5.0f);
}

TEST_F(ForestPlacementDrawTest, FinishKeepsTheNearOwnersOfASubmittedFrameAndDropsAnAbortedOne)
{
    using namespace Engine::Renderer;
    const auto file = m_Scratch.Path() / "owners.geforest";
    WriteStore(file);
    std::string error;
    auto runtime = ForestPlacementRuntime::Open(file, {.DetailSlots = 1, .ReadsPerFrame = 1}, error);
    ASSERT_TRUE(runtime) << error;
    RenderServices services;
    AssetManager assets;
    auto& feature = services.EnsureFeature<ForestPlacementFeature>();
    const auto session = feature.Attach(runtime, assets, 7, GUID::Generate(), 100.0f, 4);
    ASSERT_TRUE(session);
    const ForestMeshOwnedPlacement kept{{0, 0, 1}, 3};
    const ForestMeshOwnedPlacement planned{{0, 0, 1}, 5};
    session->NearOwners = {kept};
    session->NearOwnerView = 2;

    Rendering::RenderGraph::RGFrame aborted(nullptr, nullptr, nullptr, nullptr);
    session->PendingNearOwners = {planned};
    session->PendingNearFrame.Stamp(aborted);
    session->PendingNearView = 3;
    feature.OnFrameAbortedRG(aborted);
    EXPECT_EQ(session->NearOwners, std::vector{kept});
    EXPECT_EQ(session->NearOwnerView, 2u);
    EXPECT_TRUE(session->PendingNearOwners.empty());

    Rendering::RenderGraph::RGFrame submitted(nullptr, nullptr, nullptr, nullptr);
    session->PendingNearOwners = {planned};
    session->PendingNearFrame.Stamp(submitted);
    feature.OnFrameSubmittedRG(aborted, {});
    EXPECT_EQ(session->NearOwners, std::vector{kept});
    feature.OnFrameSubmittedRG(submitted, {});
    EXPECT_EQ(session->NearOwners, std::vector{planned});
    EXPECT_EQ(session->NearOwnerView, 3u);
    EXPECT_TRUE(session->PendingNearOwners.empty());
}

Rendering::ViewId AddForestStandView(Engine::Renderer::RenderServices& services, double x,
                                     uint64 world, Rendering::RenderLayerMask layers)
{
    using namespace Engine::Renderer;
    using namespace Mathematics;
    Rendering::CameraData camera{};
    camera.view = MakeLookAtLH({}, {0, 0, 1}, {0, 1, 0});
    camera.proj = MakeOrthographicLH_ZO_ReverseZ(-25, 25, -25, 30, 0.1f, 80.0f);
    camera.viewProj = camera.proj * camera.view;
    SetCameraEye(camera, {x, 8.0, 511'999'990.0});
    const auto cameraId = services.Views().AllocateCamera("ForestStandCamera");
    services.Views().SetCameraData(cameraId, camera, {});
    const auto view = services.Views().AllocateView("ForestStandView", cameraId);
    services.Views().SetViewWorldId(view, world);
    services.Views().SetViewRenderLayerMask(view, layers);
    return view;
}

Engine::Renderer::ForestStandDrawHeads MakeForestStandDrawHeads(bool includeDepth)
{
    Engine::Renderer::ForestStandDrawHeads heads{};
    heads.Color.FirstIndex = 512;
    heads.Color.VertexOffset = 19;
    heads.Color.IndexCount = 12;
    if (includeDepth)
        heads.Depth = heads.Color;
    return heads;
}

TEST_F(ForestPlacementDrawTest, EmitsStandRangesAndMatchingDepthOnlyIntoEligibleViews)
{
    using namespace Engine::Renderer;
    using namespace Mathematics;

    const auto file = m_Scratch.Path() / "valid.geforest";
    WriteStore(file);
    std::string error;
    std::shared_ptr<ForestPlacementRuntime> runtime = ForestPlacementRuntime::Open(file, {}, error);
    ASSERT_TRUE(runtime) << error;

    RenderServices services;
    constexpr uint64 kForestWorld = 45;
    const auto defaultLayer = Rendering::RenderLayerMask::Default();
    const auto first = AddForestStandView(services, 512'000'000.0, kForestWorld, defaultLayer);
    const auto second = AddForestStandView(services, 512'000'055.0, kForestWorld, defaultLayer);
    const auto missed = AddForestStandView(services, 512'000'500.0, kForestWorld, defaultLayer);
    const auto otherWorld = AddForestStandView(services, 512'000'000.0, kForestWorld + 1, defaultLayer);
    const auto hiddenLayer = AddForestStandView(services, 512'000'000.0, kForestWorld,
                                               Rendering::RenderLayerMask::FromLayer(2));
    unsigned prepared = 0;
    const auto subscription = RegisterForestStandDraws(services, kForestWorld, defaultLayer, runtime,
        [&prepared, first](const ForwardEmitContext& context) -> std::optional<ForestStandDrawHeads>
        {
            ++prepared;
            return MakeForestStandDrawHeads(context.ViewId == first);
        });

    Rendering::RenderGraph::RGFrame graph(nullptr, nullptr, nullptr, nullptr);
    for (const auto view : {first, second, missed, otherWorld, hiddenLayer, first})
        services.EmitFrameForwardCommandsForView(graph, view);

    const size_t prepass = static_cast<size_t>(DepthPassType::Prepass);
    const auto& firstView = services.Views().PerView(first);
    const auto& secondView = services.Views().PerView(second);
    ASSERT_EQ(firstView.ForwardCommands.size(), 1u);
    ASSERT_EQ(secondView.ForwardCommands.size(), 1u);
    ASSERT_EQ(firstView.DepthCommands[prepass].size(), 1u);
    EXPECT_TRUE(secondView.DepthCommands[prepass].empty());
    const auto firstDraws = firstView.ForwardCommands.front().SubDraws;
    const auto secondDraws = secondView.ForwardCommands.front().SubDraws;
    const auto depthDraws = firstView.DepthCommands[prepass].front().SubDraws;
    ASSERT_EQ(firstDraws.size(), 1u);
    ASSERT_EQ(secondDraws.size(), 1u);
    EXPECT_EQ(firstDraws.front().FirstIndex, 512u);
    EXPECT_EQ(firstDraws.front().VertexOffset, 19);
    EXPECT_EQ(firstDraws.front().IndexCount, 6u);
    EXPECT_EQ(secondDraws.front().FirstIndex, 518u);
    EXPECT_EQ(secondDraws.front().VertexOffset, 19);
    EXPECT_EQ(secondDraws.front().IndexCount, 6u);
    EXPECT_EQ(depthDraws.data(), firstDraws.data());
    EXPECT_EQ(depthDraws.size(), firstDraws.size());
    EXPECT_EQ(prepared, 2u);
    for (const auto view : {missed, otherWorld, hiddenLayer})
        EXPECT_TRUE(services.Views().PerView(view).ForwardCommands.empty());
}

TEST_F(ForestPlacementDrawTest, SplitsAVisibleOverviewRangeAtThePageSectorBoundary)
{
    using namespace Engine::Renderer;
    constexpr double boundary = 512'000'512.0;
    m_Pages[0].Origin.X = boundary - 4.0;
    m_Pages[1].Origin.X = boundary + 4.0;
    const auto file = m_Scratch.Path() / "sector.geforest";
    WriteStore(file);
    std::string error;
    auto runtime = ForestPlacementRuntime::Open(file, {}, error);
    ASSERT_TRUE(runtime) << error;

    RenderServices services;
    const auto layers = Rendering::RenderLayerMask::Default();
    const auto view = AddForestStandView(services, boundary, 45, layers);
    const auto subscription = RegisterForestStandDraws(services, 45, layers, runtime,
        [](const ForwardEmitContext&) -> std::optional<ForestStandDrawHeads>
        {
            auto heads = MakeForestStandDrawHeads(true);
            heads.SectorLocalMaterialIndex = 17;
            return heads;
        });
    Rendering::RenderGraph::RGFrame graph(nullptr, nullptr, nullptr, nullptr);
    for (const auto purpose : {ForwardEmitPurpose::World, ForwardEmitPurpose::ReflectionProbeCapture,
                               ForwardEmitPurpose::PlanarReflectionCapture})
    {
        if (purpose == ForwardEmitPurpose::World)
            services.EmitFrameForwardCommandsForView(graph, view);
        else
            services.EmitProducerForwardCommandsForView(view, purpose, &graph);
        const auto& state = services.Views().PerView(view);
        const auto& depth = state.DepthCommands[static_cast<size_t>(DepthPassType::Prepass)];
        ASSERT_EQ(state.ForwardCommands.size(), 2u);
        const bool expectsCameraDepth = purpose == ForwardEmitPurpose::World;
        ASSERT_EQ(depth.size(), expectsCameraDepth ? 2u : 0u);
        for (uint32 page = 0; page < 2; ++page)
        {
            const auto& command = state.ForwardCommands[page];
            ASSERT_EQ(command.SubDraws.size(), 1u);
            EXPECT_EQ(command.SubDraws.front().FirstIndex, 512u + 6u * page);
            EXPECT_EQ(command.SubDraws.front().IndexCount, 6u);
            if (expectsCameraDepth)
                EXPECT_EQ(command.Bindings.PushConstants.data(), depth[page].Bindings.PushConstants.data());
            ASSERT_EQ(command.Bindings.PushConstants.size(), sizeof(IdentityMeshDrawConstants));
            IdentityMeshDrawConstants constants(0);
            std::memcpy(&constants, command.Bindings.PushConstants.data(), sizeof(constants));
            EXPECT_EQ(constants.HasSector, 1u);
            EXPECT_EQ(constants.MaterialIndex, 17u);
            EXPECT_EQ(std::bit_cast<int32>(constants.NormalColumn0.w), 500000 + static_cast<int32>(page));
            EXPECT_EQ(std::bit_cast<int32>(constants.NormalColumn2.w), 500000);
        }
    }
}

TEST_F(ForestPlacementDrawTest, ReflectionCapturesDrawTheOverviewWithoutCameraDepthHeads)
{
    using namespace Engine::Renderer;
    const auto file = m_Scratch.Path() / "reflection.geforest";
    WriteStore(file);
    std::string error;
    const auto runtime = ForestPlacementRuntime::Open(file, {}, error);
    ASSERT_TRUE(runtime) << error;

    RenderServices services;
    constexpr uint64 kWorld = 45;
    const auto layer = Rendering::RenderLayerMask::Default();
    const auto capture = AddForestStandView(services, 512'000'055.0, kWorld, layer);
    const auto otherWorld = AddForestStandView(services, 512'000'055.0, kWorld + 1, layer);
    const auto subscription = RegisterForestStandDraws(services, kWorld, layer, runtime,
        [](const ForwardEmitContext&) { return MakeForestStandDrawHeads(true); });

    Rendering::RenderGraph::RGFrame graph(nullptr, nullptr, nullptr, nullptr);
    for (const auto purpose : {ForwardEmitPurpose::ReflectionProbeCapture, ForwardEmitPurpose::PlanarReflectionCapture})
    {
        services.EmitProducerForwardCommandsForView(capture, purpose, &graph);
        services.EmitProducerForwardCommandsForView(otherWorld, purpose, &graph);
        const auto& commands = services.Views().PerView(capture);
        ASSERT_EQ(commands.ForwardCommands.size(), 1u);
        const auto draws = commands.ForwardCommands.front().SubDraws;
        ASSERT_EQ(draws.size(), 1u);
        EXPECT_EQ(draws.front().FirstIndex, 518u);
        EXPECT_TRUE(commands.DepthCommands[static_cast<size_t>(DepthPassType::Prepass)].empty());
        EXPECT_TRUE(services.Views().PerView(otherWorld).ForwardCommands.empty());
    }
}

TEST_F(ForestPlacementDrawTest, AStandDrawsNothingWhenItsPagesReachPastTheOverviewMesh)
{
    using namespace Engine::Renderer;
    const auto file = m_Scratch.Path() / "short.geforest";
    WriteStore(file);
    std::string error;
    std::shared_ptr<ForestPlacementRuntime> runtime = ForestPlacementRuntime::Open(file, {}, error);
    ASSERT_TRUE(runtime) << error;

    RenderServices services;
    constexpr uint64 kForestWorld = 46;
    const auto defaultLayer = Rendering::RenderLayerMask::Default();
    // This view sees the second page, which addresses indices 6 to 12.
    const auto view = AddForestStandView(services, 512'000'055.0, kForestWorld, defaultLayer);
    const auto subscription = RegisterForestStandDraws(services, kForestWorld, defaultLayer, runtime,
        [](const ForwardEmitContext&) -> std::optional<ForestStandDrawHeads>
        {
            auto heads = MakeForestStandDrawHeads(false);
            heads.Color.IndexCount = 6;
            return heads;
        });

    Rendering::RenderGraph::RGFrame graph(nullptr, nullptr, nullptr, nullptr);
    services.EmitFrameForwardCommandsForView(graph, view);
    EXPECT_TRUE(services.Views().PerView(view).ForwardCommands.empty());
}

TEST_F(ForestPlacementDrawTest, AStandDrawsItsOverviewWhenItsSessionDrawsNoDetails)
{
    using namespace Engine::Renderer;
    const auto file = m_Scratch.Path() / "fallback.geforest";
    WriteStore(file);
    std::string error;
    std::shared_ptr<ForestPlacementRuntime> runtime =
        ForestPlacementRuntime::Open(file, {.DetailSlots = 1, .ReadsPerFrame = 1}, error);
    ASSERT_TRUE(runtime) << error;
    RenderServices services;
    AssetManager assets;
    constexpr uint64 kForestWorld = 45;
    const auto defaultLayer = Rendering::RenderLayerMask::Default();
    const auto view = AddForestStandView(services, 512'000'000.0, kForestWorld, defaultLayer);
    // Without a device or a card, the session declares nothing into the view.
    const auto session = services.EnsureFeature<ForestPlacementFeature>().Attach(
        runtime, assets, kForestWorld, GUID::Generate(), 100.0f);
    ASSERT_TRUE(session);
    const auto subscription = RegisterForestStandDraws(services, kForestWorld, defaultLayer, runtime,
        [](const ForwardEmitContext&) -> std::optional<ForestStandDrawHeads> { return MakeForestStandDrawHeads(false); },
        session);

    Rendering::RenderGraph::RGFrame graph(nullptr, nullptr, nullptr, nullptr);
    services.EmitFrameForwardCommandsForView(graph, view);
    const auto& commands = services.Views().PerView(view).ForwardCommands;
    ASSERT_EQ(commands.size(), 1u);
    ASSERT_EQ(commands.front().SubDraws.size(), 1u);
    EXPECT_EQ(commands.front().SubDraws.front().FirstIndex, 512u);
    EXPECT_EQ(commands.front().SubDraws.front().IndexCount, 6u);
    EXPECT_FALSE(session->Stream);
}

TEST_F(ForestPlacementDrawTest, AStandWhoseSessionDrawsNoColorEmitsNoColor)
{
    using namespace Engine::Renderer;
    const auto file = m_Scratch.Path() / "shadows-only.geforest";
    WriteStore(file);
    std::string error;
    std::shared_ptr<ForestPlacementRuntime> runtime =
        ForestPlacementRuntime::Open(file, {.DetailSlots = 1, .ReadsPerFrame = 1}, error);
    ASSERT_TRUE(runtime) << error;
    RenderServices services;
    AssetManager assets;
    constexpr uint64 kForestWorld = 45;
    const auto defaultLayer = Rendering::RenderLayerMask::Default();
    const auto view = AddForestStandView(services, 512'000'000.0, kForestWorld, defaultLayer);
    const auto session = services.EnsureFeature<ForestPlacementFeature>().Attach(
        runtime, assets, kForestWorld, GUID::Generate(), 100.0f);
    ASSERT_TRUE(session);
    session->DrawsColor = false;
    const auto subscription = RegisterForestStandDraws(services, kForestWorld, defaultLayer, runtime,
        [](const ForwardEmitContext&) -> std::optional<ForestStandDrawHeads> { return MakeForestStandDrawHeads(false); },
        session);

    Rendering::RenderGraph::RGFrame graph(nullptr, nullptr, nullptr, nullptr);
    services.EmitFrameForwardCommandsForView(graph, view);
    EXPECT_TRUE(services.Views().PerView(view).ForwardCommands.empty());
}

TEST_F(ForestPlacementDrawTest, SuppressesOnlyCardsOwnedByTheMatchingMeshSubmission)
{
    using namespace Engine::Renderer;
    using namespace Mathematics;

    const auto file = m_Scratch.Path() / "mesh-ownership.geforest";
    std::array<PackedPlacement, PackedPlacementStore::kPlacementsPerPage> all{};
    auto page = m_Pages[0];
    page.Count = static_cast<uint32>(all.size());
    ASSERT_TRUE(WritePackedPlacementStore(file, kTestBoundRadius, std::span(&page, 1), all).empty());
    std::string error;
    auto runtime = ForestPlacementRuntime::Open(file, {.DetailSlots = 1, .ReadsPerFrame = 1}, error);
    ASSERT_TRUE(runtime) << error;
    const ForestPageTicket ticket{0, 0, 14};
    ForestPlacementFrame plan;
    plan.DetailedDraws.push_back({ticket, 0, page.Count});
    const std::array claims{
        ForestMeshOwnedPlacement{ticket, 0}, ForestMeshOwnedPlacement{ticket, 31},
        ForestMeshOwnedPlacement{ticket, 32}, ForestMeshOwnedPlacement{ticket, 127},
        ForestMeshOwnedPlacement{ticket, 128}, ForestMeshOwnedPlacement{ticket, 143},
        ForestMeshOwnedPlacement{{0, 0, 13}, 1}, // Stale mesh from the preceding slot generation.
        ForestMeshOwnedPlacement{{1, 0, 14}, 2}, // Wrong slot.
        ForestMeshOwnedPlacement{{0, 1, 14}, 3}, // No detail draw for this page.
        ForestMeshOwnedPlacement{ticket, 144},   // Outside the placement packet.
    };
    Rendering::CameraData camera{};
    SetCameraEye(camera, {512'000'000.0, 8.0, 511'999'990.0});
    ForestPlacementGpuView gpu{};
    std::vector<ForestPlacementGpuPage> pages;
    ASSERT_TRUE(BuildForestPlacementGpuInputs(*runtime, plan.DetailedDraws,
        *MakeForestPlacementGpuViewSource(camera), 1, 15.0f, {0, 7, 0, 8}, gpu, pages, claims));
    ASSERT_EQ(pages.size(), 1u);
    constexpr std::array<uint32, 8> mask{0x80000001u, 1u, 0u, 0x80000000u, 0x8001u, 0u, 0u, 0u};
    EXPECT_EQ(pages.front().MeshOwned, mask);

    ASSERT_TRUE(BuildForestPlacementGpuInputs(*runtime, plan.DetailedDraws,
        *MakeForestPlacementGpuViewSource(camera), 1, 15.0f, {0, 7, 0, 8}, gpu, pages));
    ASSERT_EQ(pages.size(), 1u);
    EXPECT_EQ(pages.front().MeshOwned, (std::array<uint32, 8>{}));
}

TEST_F(ForestPlacementDrawTest, TheColorCardViewerIsTheEyePoint)
{
    using namespace Engine::Renderer;
    // The color card faces the eye that the placement view of the same camera holds.
    Rendering::CameraData camera{};
    SetCameraEye(camera, {512'000'054.25, 8.5, 511'999'990.75});
    const auto source = MakeForestPlacementGpuViewSource(camera);
    ASSERT_TRUE(source);
    const auto viewer = ForestCardEyeViewer(camera);
    EXPECT_EQ(viewer.x, source->EyeRelative.x);
    EXPECT_EQ(viewer.y, source->EyeRelative.y);
    EXPECT_EQ(viewer.z, source->EyeRelative.z);
    EXPECT_EQ(viewer.w, 1.0f);
}

TEST_F(ForestPlacementDrawTest, BuildsPackedGpuPagesInTheCamerasRenderOrigin)
{
    using namespace Engine::Renderer;
    using namespace Mathematics;
    const auto file = m_Scratch.Path() / "valid.geforest";
    WriteStore(file);
    std::string error;
    auto runtime = ForestPlacementRuntime::Open(file, {.DetailSlots = 1, .ReadsPerFrame = 1}, error);
    ASSERT_TRUE(runtime) << error;
    const WorldPosition position{512'000'054.0, 8.0, 511'999'990.0};
    const ForestPlacementView visible{position, {}, 100.0f};
    ForestPlacementFrame plan;
    ASSERT_TRUE(runtime->Prepare(visible, 1, 0, {}, plan));
    ASSERT_EQ(plan.Reads.size(), 1u);
    const auto ticket = plan.Reads.front();
    ASSERT_EQ(ticket.Page, 1u);
    ASSERT_TRUE(runtime->CompleteRead(ticket, kSecondPage));
    ASSERT_TRUE(runtime->CompleteUpload(ticket, ForestUploadResult::Published));
    ASSERT_TRUE(runtime->Prepare(visible, 2, 1, {}, plan));

    Rendering::CameraData camera{};
    camera.view = MakeLookAtLH({}, {0, 0, 1}, {0, 1, 0});
    camera.proj = MakeOrthographicLH_ZO_ReverseZ(-30, 30, -25, 30, 0.1f, 80.0f);
    camera.viewProj = camera.proj * camera.view;
    SetCameraEye(camera, position);
    ForestPlacementGpuView gpu{};
    std::vector<ForestPlacementGpuPage> pages;
    ASSERT_TRUE(BuildForestPlacementGpuInputs(*runtime, plan.DetailedDraws,
        *MakeForestPlacementGpuViewSource(camera), 1, 15.0f, {0, 7, 0, 8}, gpu, pages));
    ASSERT_EQ(pages.size(), 1u);
    EXPECT_EQ(pages[0].Range, (std::array<uint32, 4>{0, 2, 1, 0}));
    EXPECT_FLOAT_EQ(pages[0].OriginRelative.x - gpu.EyeRelativeAndRadius.x, 1.0f);
    EXPECT_FLOAT_EQ(pages[0].OriginRelative.z - gpu.EyeRelativeAndRadius.z, 12.0f);
    EXPECT_FLOAT_EQ(gpu.EyeRelativeAndRadius.w, 15.0f);
    EXPECT_FLOAT_EQ(gpu.AtlasBounds.x, 0.0f);
    EXPECT_FLOAT_EQ(gpu.AtlasBounds.y, 7.0f);
    EXPECT_FLOAT_EQ(gpu.AtlasBounds.z, 0.0f);
    EXPECT_FLOAT_EQ(gpu.AtlasBounds.w, 8.0f);
    auto missing = plan;
    ++missing.DetailedDraws.front().Ticket.Generation;
    ASSERT_TRUE(BuildForestPlacementGpuInputs(*runtime, missing.DetailedDraws,
        *MakeForestPlacementGpuViewSource(camera), 1, 15.0f, {0, 7, 0, 8}, gpu, pages));
    EXPECT_EQ(pages[0].Range[2], 0u);

    camera.proj = MakeOrthographicLH_ZO_ReverseZ(-1, 1.5f, -25, 30, 0.1f, 80.0f);
    camera.viewProj = camera.proj * camera.view;
    SetCameraEye(camera, position);
    ASSERT_TRUE(BuildForestPlacementGpuInputs(*runtime, plan.DetailedDraws,
        *MakeForestPlacementGpuViewSource(camera), 1, 15.0f, {0, 7, 0, 8}, gpu, pages));
    ASSERT_EQ(pages.size(), 1u);
    EXPECT_EQ(pages[0].Range[2], 0u); // A verified root lies outside the right edge.

    // Page 1's roots stand 0 and 0.23 meters above its origin, at the eye's height.
    camera.proj = MakeOrthographicLH_ZO_ReverseZ(-30, 30, -25, 0.3f, 0.1f, 80.0f);
    camera.viewProj = camera.proj * camera.view;
    SetCameraEye(camera, position);
    ASSERT_TRUE(BuildForestPlacementGpuInputs(*runtime, plan.DetailedDraws,
        *MakeForestPlacementGpuViewSource(camera), 1, 15.0f, {0, 7, 0, 8}, gpu, pages));
    EXPECT_EQ(pages[0].Range[2], 1u);
    camera.proj = MakeOrthographicLH_ZO_ReverseZ(-30, 30, -25, 0.1f, 0.1f, 80.0f);
    camera.viewProj = camera.proj * camera.view;
    SetCameraEye(camera, position);
    ASSERT_TRUE(BuildForestPlacementGpuInputs(*runtime, plan.DetailedDraws,
        *MakeForestPlacementGpuViewSource(camera), 1, 15.0f, {0, 7, 0, 8}, gpu, pages));
    EXPECT_EQ(pages[0].Range[2], 0u); // The higher root lies above the top edge.

    ++plan.DetailedDraws.front().FirstPlacement;
    EXPECT_FALSE(BuildForestPlacementGpuInputs(*runtime, plan.DetailedDraws,
        *MakeForestPlacementGpuViewSource(camera), 1, 15.0f, {0, 7, 0, 8}, gpu, pages));
    EXPECT_TRUE(pages.empty());
}


// A session on a headless device. Device-gated: each test skips without a Vulkan device.
class ForestPlacementSessionDeviceTest : public PackedPlacementArchiveTest
{
protected:
    void SetUp() override
    {
        Rendering::DeviceDesc desc{};
        desc.preferredAPI = Rendering::GraphicsAPI::Vulkan;
        desc.enableDynamicRendering = true;
        m_Device = Rendering::DeviceFactory::CreateDevice(desc);
        if (!m_Device || !m_Device->Initialize(desc))
            GTEST_SKIP() << "No Vulkan device";
        ASSERT_TRUE(m_Services.Initialize(m_Device.get()));
        m_Main = std::make_unique<Window>(m_Device.get());
        const auto file = m_Scratch.Path() / "session.geforest";
        WriteStore(file);
        std::string error;
        m_Runtime = Engine::Renderer::ForestPlacementRuntime::Open(file, {.DetailSlots = 1, .ReadsPerFrame = 1}, error);
        ASSERT_TRUE(m_Runtime) << error;
        m_Feature = &m_Services.EnsureFeature<Engine::Renderer::ForestPlacementFeature>();
        m_Session = m_Feature->Attach(m_Runtime, m_Assets, kWorld, m_Model, 100.0f);
        ASSERT_TRUE(m_Session);
        const auto camera = m_Services.Views().AllocateCamera("ForestSessionCamera");
        m_View = m_Services.Views().AllocateView("ForestSessionView", camera);
        m_SceneView = m_Services.Views().AllocateView("ForestSessionSceneView", camera, Rendering::ViewPurpose::EditorScene);
    }

    void TearDown() override
    {
        if (!m_Device)
            return;
        m_Session.reset();
        m_Device->WaitForIdle();
        m_Other.reset();
        m_Main.reset();
        m_Services.Shutdown();
        m_Device->Shutdown();
    }

    // The frame stream of one window: its graph keeps its own frame numbers.
    struct Window
    {
        explicit Window(Rendering::IDevice* device)
            : Persistent(device), Transient(device), Ring(device, 2, 256 * 1024),
              Graph(device, &Persistent, &Transient, &Ring)
        {
        }

        Rendering::RenderGraph::RGResourcePool Persistent;
        Rendering::RenderGraph::RGTransientPool Transient;
        Rendering::RenderGraph::RGUploadRing Ring;
        Rendering::RenderGraph::RGFrame Graph;
    };

    // Declares frame `frameIndex` of the window's graph into each view in turn and submits it. Call it
    // after BeginWorldDrawFrame. True where the session drew.
    std::vector<bool> EmitInto(Window& window, uint64 frameIndex, std::initializer_list<Rendering::ViewId> views)
    {
        auto& graph = window.Graph;
        EXPECT_TRUE(m_Device->BeginFrame());
        graph.BeginFrame(frameIndex);
        std::vector<bool> drew;
        for (const auto view : views)
        {
            Engine::Renderer::ForwardEmitContext context{.Device = m_Device.get(), .Services = &m_Services,
                                                         .Frame = &graph, .ViewId = view};
            std::vector<Engine::Renderer::DrawSubCommand> ranges;
            drew.push_back(m_Feature->EmitDetails(*m_Session, context, {}, m_Near, ranges));
        }
        graph.Execute();
        m_Feature->OnFrameSubmittedRG(graph, graph.SubmissionToken());
        m_Device->WaitForIdle();
        return drew;
    }

    // Starts an app frame and declares frame `frameIndex` of the main window into the views.
    std::vector<bool> Emit(uint64 frameIndex, std::initializer_list<Rendering::ViewId> views)
    {
        m_Services.BeginWorldDrawFrame();
        return EmitInto(*m_Main, frameIndex, views);
    }

    bool Emit(uint64 frameIndex) { return Emit(frameIndex, {m_View}).front(); }

    // Starts an app frame and plans frame `frameIndex` of the main window for the sun cascades of each
    // view in turn, from `view` and the near page. True where the session planned.
    std::vector<bool> PlanShadows(uint64 frameIndex, std::initializer_list<Rendering::ViewId> views,
                                  const Engine::Renderer::ForestPlacementView& view)
    {
        m_Services.BeginWorldDrawFrame();
        auto& graph = m_Main->Graph;
        EXPECT_TRUE(m_Device->BeginFrame());
        graph.BeginFrame(frameIndex);
        std::vector<bool> planned;
        for (const auto viewId : views)
            planned.push_back(m_Feature->PlanShadowFrame(*m_Session, ShadowContext(graph, viewId), view, m_ShadowPages));
        graph.Execute();
        m_Feature->OnFrameSubmittedRG(graph, graph.SubmissionToken());
        m_Device->WaitForIdle();
        return planned;
    }

    Engine::Renderer::DepthEmitContext ShadowContext(Rendering::RenderGraph::RGFrame& graph, Rendering::ViewId view)
    {
        return {.Device = m_Device.get(), .Services = &m_Services, .ViewId = view, .Frame = &graph};
    }

    // The cascade draws that one frame of the casters gave a view, and the viewer of the first card in
    // cascade 1.
    struct CasterFrame
    {
        size_t Cascade0 = 0;
        size_t Cascade1 = 0;
        bool Indirect = false;
        std::optional<Mathematics::Vector4> Viewer;
    };

    // Starts an app frame, runs the depth producers of `view` for `cascades` and submits the frame.
    CasterFrame EmitCasters(uint64 frameIndex, Rendering::ViewId view, const Engine::Renderer::DepthEmitCascades& cascades)
    {
        m_Services.BeginWorldDrawFrame();
        auto& graph = m_Main->Graph;
        EXPECT_TRUE(m_Device->BeginFrame());
        graph.BeginFrame(frameIndex);
        m_Services.EmitProducerDepthCommandsForView(view, &graph, &cascades);
        const auto& streams = m_Services.Views().PerView(view).CascadeDepthCommands;
        CasterFrame result{streams[0].size(), streams[1].size()};
        if (!streams[1].empty())
        {
            const auto& card = streams[1].front();
            result.Indirect = card.UseIndirect;
            for (const auto& buffer : card.Bindings.Buffers)
                if (buffer.Name == HashStringId("forestCard"))
                {
                    // forestCard: atlas, wind, material, then the viewer at byte 48.
                    const auto* bytes = static_cast<const std::byte*>(m_Device->MapBuffer(buffer.Buffer));
                    Mathematics::Vector4 viewer{};
                    if (bytes)
                        std::memcpy(viewer.v, bytes + buffer.Offset + 48, sizeof(viewer.v));
                    result.Viewer = viewer;
                }
        }
        graph.Execute();
        m_Feature->OnFrameSubmittedRG(graph, graph.SubmissionToken());
        m_Device->WaitForIdle();
        return result;
    }

    // A packed card material, registered as the MeshImpostor system registers one.
    GUID RegisterCardMaterial()
    {
        MaterialDocument doc{};
        doc.schemaVersion = 3;
        doc.materialName = "ForestShadowCard";
        doc.lightingModel = "StandardPBR";
        doc.surfaceShader = "Surfaces/impostor.glsl";
        doc.vertexModifier = "VertexModifiers/forest_impostor.glsl";
        doc.customVertexShader = true;
        doc.alphaMode = MaterialAlphaMode::Mask;
        doc.doubleSided = true;
        doc.properties["alphaCutoff"] = 0.5f;
        doc.properties["impostorBounds"] = std::vector<float>{0.0f, 5.0f, 0.0f, 2.0f};
        doc.textures["albedoMap"] = std::string{};
        doc.textures["normalMap"] = std::string{};
        const GUID guid = GUID::Generate();
        Engine::Renderer::Material* material = m_Services.Materials().RegisterMaterialFromDocument(guid, doc);
        if (!material)
            return {};
        // The surface's property table comes with the compile, which no frame here drains.
        constexpr std::array<float, 4> kAtlas{8.0f, 0.0f, 0.0f, 0.0f};
        Engine::Renderer::Material::TestFactory::AddPropertyLayout(*material, HashStringId("impostorAtlas"), 0u, sizeof(kAtlas));
        material->SetVector(HashStringId("impostorAtlas"), kAtlas.data(), 4u);
        return guid;
    }

    void PublishCard()
    {
        Engine::Renderer::ForestImpostorCard card{};
        card.Bounds = {0.0f, 5.0f, 0.0f, 2.0f};
        m_Feature->PublishCard(kWorld, m_Model, card);
    }

    // True when the runtime still asks for the near page: no stream has read it.
    bool NoPageWasRead()
    {
        Engine::Renderer::ForestPlacementFrame plan;
        return m_Runtime->Prepare(m_Near, 100, 0, {}, plan) && plan.Reads.size() == 1;
    }

    static constexpr uint64 kWorld = 7;
    Rendering::Tests::ScopedEnvVar m_Headless{"GE_HEADLESS_TEST", "1"};
    const Engine::Renderer::ForestPlacementView m_Near{{512'000'054.0, 8.0, 511'999'990.0}, {}, 100.0f};
    // An eye far from every page: the camera wants no trees.
    const Engine::Renderer::ForestPlacementView m_Far{{512'010'000.0, 8.0, 511'999'990.0}, {}, 100.0f};
    // A sun cascade wants the near page.
    const std::array<Engine::Renderer::ForestShadowPage, 1> m_ShadowPages{{{1, 1}}};
    const GUID m_Model = GUID::Generate();
    std::unique_ptr<Rendering::IDevice> m_Device;
    Engine::Renderer::RenderServices m_Services;
    AssetManager m_Assets;
    std::shared_ptr<Engine::Renderer::ForestPlacementRuntime> m_Runtime;
    Engine::Renderer::ForestPlacementFeature* m_Feature = nullptr;
    std::shared_ptr<Engine::Renderer::ForestPlacementSession> m_Session;
    Rendering::ViewId m_View{};
    Rendering::ViewId m_SceneView{};
    std::unique_ptr<Window> m_Main;
    std::unique_ptr<Window> m_Other;
};

TEST_F(ForestPlacementSessionDeviceTest, OpaqueOverviewPrepassBindsOnlyTheSetsItsPipelineDeclares)
{
    using namespace Engine::Renderer;
    const auto shaders = TestPaths::StagedRenderingShadersDir();
    Rendering::MaterialBuildContext context{};
    context.AdapterShaderDir = shaders;
    context.IncludeDirs = {shaders};
    context.CacheRoot = m_Scratch.Path() / "shader-cache";
    m_Services.Materials().SetMaterialBuildContext(context);

    MaterialDocument document{};
    document.materialName = "OpaqueForestOverview";
    document.lightingModel = "StandardPBR";
    document.surfaceShader = "Surfaces/standard_pbr.glsl";
    const GUID material = GUID::Generate();
    ASSERT_NE(m_Services.Materials().RegisterMaterialFromDocument(material, document), nullptr);

    Mesh mesh{};
    mesh.Vertices.resize(4);
    mesh.Vertices[1].Position[0] = 1.0f;
    mesh.Vertices[2].Position[1] = 1.0f;
    mesh.Vertices[3].Position[0] = mesh.Vertices[3].Position[1] = 1.0f;
    mesh.Indices = {0, 1, 2, 1, 3, 2, 0, 1, 2, 1, 3, 2};
    const Rendering::MeshGPUKey key{GUID::Generate(), 0};
    ASSERT_TRUE(m_Services.GetMeshGPURegistry().RegisterSubmesh(key, mesh).IsValid());
    const auto layers = Rendering::RenderLayerMask::Default();
    const auto view = AddForestStandView(m_Services, 512'000'055.0, kWorld, layers);
    const auto stand = RegisterForestStandMesh(m_Services, kWorld, layers, m_Runtime, key,
                                               [material] { return material; });
    auto& formats = m_Services.Views().PerView(view).PrepassFormatKey;
    formats.DepthFormat = Rendering::TextureFormat::D32_FLOAT;
    formats.RasterizationSamples = 1;
    for (uint32 frame = 0; frame < 8; ++frame)
    {
        m_Services.Materials().BeginFrame();
        ASSERT_TRUE(m_Device->BeginFrame());
        m_Main->Graph.BeginFrame(frame);
        m_Services.EmitProducerForwardCommandsForView(view);
        m_Services.EmitFrameForwardCommandsForView(m_Main->Graph, view);
        const auto heads = m_Services.GetDepthCommands(view, DepthPassType::Prepass);
        m_Main->Graph.Execute();
        m_Device->WaitForIdle();
        if (heads.empty())
            continue;
        ASSERT_EQ(heads.size(), 1u);
        const auto& head = heads.front();
        const auto* pipeline = m_Device->LookupGraphicsPipeline(head.InternedPipeline);
        ASSERT_NE(pipeline, nullptr);
        ASSERT_EQ(pipeline->PixelShader, nullptr);
        ASSERT_EQ(pipeline->DescriptorSetLayouts.size(), 1u);
        ASSERT_NE(head.PipelineMeta, nullptr);
        ASSERT_TRUE(std::ranges::any_of(head.PipelineMeta->Sets, [](const auto& set) {
            return set.Set > 0u && !set.Bindings.empty();
        }));
        EXPECT_EQ(head.PipelineSetCount, pipeline->DescriptorSetLayouts.size());
        return;
    }
    FAIL() << "The registered opaque overview never published its prepass head";
}

// The corner of this card reaches 10 + 4 * sqrt(2) m from the root, past the archive's 12 m.
Engine::Renderer::ForestImpostorCard OversizedCard()
{
    Engine::Renderer::ForestImpostorCard card{};
    card.Bounds = {0.0f, 10.0f, 0.0f, 4.0f};
    return card;
}

TEST_F(ForestPlacementSessionDeviceTest, ARefusedSessionAllocatesNoSlotsAndReadsNoPage)
{
    EXPECT_FALSE(Emit(1));
    EXPECT_TRUE(m_Feature->WantsCard(kWorld, m_Model));
    EXPECT_FALSE(m_Session->Slots.IsValid());
    EXPECT_FALSE(m_Session->Stream);
    EXPECT_TRUE(NoPageWasRead());
    m_Feature->PublishCard(kWorld, m_Model, OversizedCard());
    EXPECT_FALSE(Emit(2));
    EXPECT_FALSE(m_Session->Slots.IsValid());
    EXPECT_FALSE(m_Session->Stream);
}

TEST_F(ForestPlacementSessionDeviceTest, ARefusedSessionDoesNotAskForItsCardAgain)
{
    Emit(1);
    m_Feature->PublishCard(kWorld, m_Model, OversizedCard());
    Emit(2);
    Emit(3);
    EXPECT_FALSE(m_Feature->WantsCard(kWorld, m_Model));
}

TEST_F(ForestPlacementSessionDeviceTest, RefusingAStreamingSessionFreesItsSlotsAndStream)
{
    Emit(1);
    Engine::Renderer::ForestImpostorCard card{};
    card.Bounds = {0.0f, 5.0f, 0.0f, 2.0f};
    m_Feature->PublishCard(kWorld, m_Model, card);
    Emit(2);
    ASSERT_TRUE(m_Session->Slots.IsValid());
    ASSERT_TRUE(m_Session->Stream);
    m_Feature->PublishCard(kWorld, m_Model, OversizedCard());
    EXPECT_FALSE(m_Session->Slots.IsValid());
    EXPECT_FALSE(m_Session->Indices.IsValid());
    EXPECT_FALSE(m_Session->Stream);
}

TEST_F(ForestPlacementSessionDeviceTest, WhilePlayingTheGameViewTakesTheDetailsFromTheSceneView)
{
    m_Services.Views().SetWorldSimulating(kWorld, true);
    Emit(1);
    Engine::Renderer::ForestImpostorCard card{};
    card.Bounds = {0.0f, 5.0f, 0.0f, 2.0f};
    m_Feature->PublishCard(kWorld, m_Model, card);
    EXPECT_EQ(Emit(2, {m_View, m_SceneView}), (std::vector{true, false}));
    EXPECT_EQ(Emit(3, {m_SceneView, m_View}), (std::vector{false, true}));
    // The Scene View waits one frame for a Game view that may still draw.
    EXPECT_EQ(Emit(4, {m_SceneView}), (std::vector{false}));
    EXPECT_EQ(Emit(5, {m_SceneView}), (std::vector{true}));
    EXPECT_EQ(Emit(6, {m_View, m_SceneView}), (std::vector{true, false}));
}

TEST_F(ForestPlacementSessionDeviceTest, WhileEditingTheSceneViewTakesTheDetailsFromTheGameView)
{
    Emit(1);
    Engine::Renderer::ForestImpostorCard card{};
    card.Bounds = {0.0f, 5.0f, 0.0f, 2.0f};
    m_Feature->PublishCard(kWorld, m_Model, card);
    EXPECT_EQ(Emit(2, {m_SceneView, m_View}), (std::vector{true, false}));
    EXPECT_EQ(Emit(3, {m_View, m_SceneView}), (std::vector{false, true}));
    // The Game view waits one frame for a Scene View that may still draw.
    EXPECT_EQ(Emit(4, {m_View}), (std::vector{false}));
    EXPECT_EQ(Emit(5, {m_View}), (std::vector{true}));
    EXPECT_EQ(Emit(6, {m_SceneView, m_View}), (std::vector{true, false}));
}

TEST_F(ForestPlacementSessionDeviceTest, AGameViewInItsOwnWindowTakesTheDetailsFromTheSceneView)
{
    // Each window counts its own frames: the Scene View window is far ahead of the Game view window.
    m_Services.Views().SetWorldSimulating(kWorld, true);
    m_Other = std::make_unique<Window>(m_Device.get());
    Emit(100, {m_SceneView});
    Engine::Renderer::ForestImpostorCard card{};
    card.Bounds = {0.0f, 5.0f, 0.0f, 2.0f};
    m_Feature->PublishCard(kWorld, m_Model, card);
    EXPECT_EQ(Emit(101, {m_SceneView}), (std::vector{true}));
    EXPECT_EQ(m_Session->Graph, &m_Main->Graph);

    m_Services.BeginWorldDrawFrame();
    EXPECT_EQ(EmitInto(*m_Main, 102, {m_SceneView}), (std::vector{true}));
    EXPECT_EQ(EmitInto(*m_Other, 1, {m_View}), (std::vector{true}));
    EXPECT_EQ(m_Session->Graph, &m_Other->Graph);
    EXPECT_TRUE(m_Session->Card);

    m_Services.BeginWorldDrawFrame();
    EXPECT_EQ(EmitInto(*m_Main, 103, {m_SceneView}), (std::vector{false}));
    EXPECT_EQ(EmitInto(*m_Other, 2, {m_View}), (std::vector{true}));
}

TEST_F(ForestPlacementSessionDeviceTest, ASessionStaysOnItsGraphWhileItsFrameIsOpen)
{
    // The Game view in another window cannot move the session while the Scene View frame is open.
    m_Services.Views().SetWorldSimulating(kWorld, true);
    m_Other = std::make_unique<Window>(m_Device.get());
    Emit(1, {m_SceneView});
    Engine::Renderer::ForestImpostorCard card{};
    card.Bounds = {0.0f, 5.0f, 0.0f, 2.0f};
    m_Feature->PublishCard(kWorld, m_Model, card);
    EXPECT_EQ(Emit(2, {m_SceneView}), (std::vector{true}));

    m_Services.BeginWorldDrawFrame();
    EXPECT_TRUE(m_Device->BeginFrame());
    m_Main->Graph.BeginFrame(3);
    std::vector<Engine::Renderer::DrawSubCommand> ranges;
    Engine::Renderer::ForwardEmitContext scene{.Device = m_Device.get(), .Services = &m_Services,
                                               .Frame = &m_Main->Graph, .ViewId = m_SceneView};
    EXPECT_TRUE(m_Feature->EmitDetails(*m_Session, scene, {}, m_Near, ranges));
    m_Other->Graph.BeginFrame(1);
    Engine::Renderer::ForwardEmitContext game{.Device = m_Device.get(), .Services = &m_Services,
                                              .Frame = &m_Other->Graph, .ViewId = m_View};
    EXPECT_FALSE(m_Feature->EmitDetails(*m_Session, game, {}, m_Near, ranges));
    EXPECT_EQ(m_Session->Graph, &m_Main->Graph);
    EXPECT_TRUE(m_Session->Stream);
    m_Main->Graph.Execute();
    m_Feature->OnFrameSubmittedRG(m_Main->Graph, m_Main->Graph.SubmissionToken());
    m_Other->Graph.Execute();
    m_Feature->OnFrameSubmittedRG(m_Other->Graph, m_Other->Graph.SubmissionToken());
    m_Device->WaitForIdle();
}

TEST_F(ForestPlacementSessionDeviceTest, TheShadowEmitPlansOnceAndTheColorEmitReusesThePlan)
{
    Emit(1);
    PublishCard();
    m_Services.BeginWorldDrawFrame();
    auto& graph = m_Main->Graph;
    EXPECT_TRUE(m_Device->BeginFrame());
    graph.BeginFrame(2);
    EXPECT_TRUE(m_Feature->PlanShadowFrame(*m_Session, ShadowContext(graph, m_View), m_Near, m_ShadowPages));
    const auto reads = m_Session->Plan.Reads;
    EXPECT_FALSE(reads.empty());

    // The color emit of the same view draws the plan: a second Prepare in the frame would fail.
    Engine::Renderer::ForwardEmitContext color{.Device = m_Device.get(), .Services = &m_Services,
                                               .Frame = &graph, .ViewId = m_View};
    std::vector<Engine::Renderer::DrawSubCommand> ranges;
    EXPECT_TRUE(m_Feature->EmitDetails(*m_Session, color, {}, m_Near, ranges));
    EXPECT_EQ(m_Session->Plan.Reads, reads);
    // Each emit declares its draws once in a frame.
    EXPECT_FALSE(m_Feature->PlanShadowFrame(*m_Session, ShadowContext(graph, m_View), m_Near, m_ShadowPages));
    EXPECT_FALSE(m_Feature->EmitDetails(*m_Session, color, {}, m_Near, ranges));
    graph.Execute();
    m_Feature->OnFrameSubmittedRG(graph, graph.SubmissionToken());
    m_Device->WaitForIdle();
}

TEST_F(ForestPlacementSessionDeviceTest, ShadowDemandAloneKeepsTheCard)
{
    EXPECT_EQ(PlanShadows(1, {m_View}, m_Far), (std::vector{false}));
    EXPECT_TRUE(m_Feature->WantsCard(kWorld, m_Model));
    PublishCard();
    EXPECT_EQ(PlanShadows(2, {m_View}, m_Far), (std::vector{true}));
    EXPECT_TRUE(m_Feature->WantsCard(kWorld, m_Model));
}

TEST_F(ForestPlacementSessionDeviceTest, OnlyTheDetailViewCastsForestShadows)
{
    // While editing, the Scene View draws the trees, so only the Scene View casts their shadows.
    Emit(1, {m_SceneView});
    PublishCard();
    EXPECT_EQ(PlanShadows(2, {m_View, m_SceneView}, m_Near), (std::vector{false, true}));
    m_Services.Views().SetWorldSimulating(kWorld, true);
    EXPECT_EQ(PlanShadows(3, {m_SceneView, m_View}, m_Near), (std::vector{false, true}));
}

TEST_F(ForestPlacementSessionDeviceTest, TheCastersDrawLightFacingCardsIntoTheCascadesThatHoldThePage)
{
    using namespace Engine::Renderer;
    const auto layers = Rendering::RenderLayerMask::Default();
    const auto view = AddForestStandView(m_Services, 512'000'054.0, kWorld, layers);
    const auto casters = RegisterForestShadowCasters(m_Services, kWorld, layers, m_Session);
    ASSERT_TRUE(casters);
    ForestImpostorCard card{};
    card.Material = RegisterCardMaterial();
    ASSERT_FALSE(card.Material.IsNull());
    card.Bounds = {0.0f, 5.0f, 0.0f, 2.0f};

    // Two cascades: cascade 0 holds nothing, cascade 1 holds every page.
    const auto* camera = m_Services.Views().FindCameraData(m_Services.Views().FindViewDesc(view)->cameraId);
    ASSERT_NE(camera, nullptr);
    DepthEmitCascades cascades{};
    cascades.RenderOriginSector = CameraRenderOriginSector(*camera);
    cascades.TowardLight = {0.0f, 1.0f, 0.0f};
    cascades.Count = 2;
    for (auto& cascade : cascades.Cascades)
        cascade.CasterPlanes.fill({0.0f, 0.0f, 0.0f, 1.0f});
    cascades.Cascades[0].CasterPlanes[0] = {0.0f, 0.0f, 0.0f, -1.0f};

    // The first frame asks for the card and the second reads the near page, which the test makes resident.
    EmitCasters(1, view, cascades);
    m_Feature->PublishCard(kWorld, m_Model, card);
    EmitCasters(2, view, cascades);
    ASSERT_EQ(m_Session->Plan.Reads.size(), 1u);
    const auto ticket = m_Session->Plan.Reads.front();
    ASSERT_TRUE(m_Runtime->CompleteRead(ticket, kSecondPage));
    ASSERT_TRUE(m_Runtime->CompleteUpload(ticket, Engine::Renderer::ForestUploadResult::Published));

    // The cull pipeline builds in the background: the cards draw once it is warm.
    CasterFrame drawn{};
    uint64 frame = 3;
    for (constexpr uint64 kLastFrame = 200; frame < kLastFrame && drawn.Cascade1 == 0; ++frame)
        drawn = EmitCasters(frame, view, cascades);
    EXPECT_EQ(drawn.Cascade0, 0u);
    ASSERT_EQ(drawn.Cascade1, 1u);
    EXPECT_TRUE(drawn.Indirect);
    ASSERT_TRUE(drawn.Viewer);
    EXPECT_EQ(drawn.Viewer->x, 0.0f);
    EXPECT_EQ(drawn.Viewer->y, 1.0f);
    EXPECT_EQ(drawn.Viewer->z, 0.0f);
    EXPECT_EQ(drawn.Viewer->w, 0.0f) << "a cascade card faces a direction";

    // Shadow Casting Off: the stand casts nothing.
    m_Session->CastsShadows = false;
    const auto off = EmitCasters(frame, view, cascades);
    EXPECT_EQ(off.Cascade0, 0u);
    EXPECT_EQ(off.Cascade1, 0u);
}

} // namespace
} // namespace GameEngine::PageStreaming
