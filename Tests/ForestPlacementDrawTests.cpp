#include "PackedPlacementTestArchive.h"

#include "Assets/AssetManager.h"

#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ForestNearMeshCandidates.h"
#include "ForestPlacementGpuData.h"
#include "ForestPlacementSession.h"
#include "ForestPlacementRuntime.h"
#include "ForestStandDrawProducer.h"
#include "ForestPlacementFeature.h"
#include "EngineLogCapture.h"
#include "Engine/Rendering/Impostors/ImpostorStage.h"
#include "Engine/Rendering/RenderOrigin.h"
#include "Engine/Rendering/RenderServices.h"
#include "Mathematics/MatrixOps.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/GPUScene.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Tests/ScopedEnvVar.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <memory>
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
    ASSERT_TRUE(runtime->Prepare(view, 1, 0, frame));
    ASSERT_EQ(frame.Reads.size(), 1u);
    const auto ticket = frame.Reads.front();
    ASSERT_EQ(ticket.Page, 0u);
    ASSERT_TRUE(runtime->CompleteRead(ticket, kFirstPage));
    ASSERT_TRUE(runtime->CompleteUpload(ticket, ForestUploadResult::Published));

    ForestNearMeshView near{view, 2.0f, 0.0f, 0.0f};
    const Mathematics::Vector4 bounds{0, 1, 0, 2};
    std::vector<ForestNearMeshCandidate> candidates;
    ASSERT_TRUE(runtime->Prepare(view, 2, 1, frame));
    CollectForestNearMeshCandidates(*runtime, frame, near, bounds, 1, candidates);
    ASSERT_EQ(candidates.size(), 1u);
    const float restCoverage = candidates.front().Coverage;

    // Outside the rest sphere even with the shared 1.5x draw guard band.
    near.PlacementView.EyeRelativePlanes[0] = {1, 0, 0, -3.7f};
    ASSERT_TRUE(runtime->Prepare(near.PlacementView, 3, 2, frame));
    ASSERT_EQ(frame.DetailedDraws.size(), 1u);
    CollectForestNearMeshCandidates(*runtime, frame, near, bounds, 1, candidates);
    EXPECT_TRUE(candidates.empty());
    near.DeformationMargin = 0.8f;
    CollectForestNearMeshCandidates(*runtime, frame, near, bounds, 1, candidates);
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
    ASSERT_TRUE(runtime->Prepare(view, 1, 0, frame));
    ASSERT_EQ(frame.Reads.size(), 1u);
    const auto ticket = frame.Reads.front();
    ASSERT_EQ(ticket.Page, 0u);
    ASSERT_TRUE(runtime->CompleteRead(ticket, kFirstPage));
    ASSERT_TRUE(runtime->CompleteUpload(ticket, ForestUploadResult::Published));
    ASSERT_TRUE(runtime->Prepare(view, 2, 1, frame));

    const Mathematics::Vector4 bounds{0, 1, 0, 2};
    ForestNearMeshView near{view, 2.0f, 0.0f, 0.0f};
    std::vector<ForestNearMeshCandidate> candidates;
    CollectForestNearMeshCandidates(*runtime, frame, near, bounds, 1, candidates);
    ASSERT_EQ(candidates.size(), 1u);
    const float coverage = candidates.front().Coverage;
    const ForestMeshOwnedPlacement current{ticket, candidates.front().LocalPlacement};

    near.ImpostorCoverage = coverage * 0.95f;
    CollectForestNearMeshCandidates(*runtime, frame, near, bounds, 1, candidates);
    EXPECT_TRUE(candidates.empty());
    CollectForestNearMeshCandidates(*runtime, frame, near, bounds, 1, candidates, std::span(&current, 1));
    ASSERT_EQ(candidates.size(), 1u);

    auto retired = current;
    ++retired.Ticket.Generation;
    CollectForestNearMeshCandidates(*runtime, frame, near, bounds, 1, candidates, std::span(&retired, 1));
    EXPECT_TRUE(candidates.empty());

    near.ImpostorCoverage = coverage * 1.01f;
    CollectForestNearMeshCandidates(*runtime, frame, near, bounds, 1, candidates, std::span(&current, 1));
    EXPECT_TRUE(candidates.empty());
    near.ImpostorCoverage = coverage * 0.75f;
    CollectForestNearMeshCandidates(*runtime, frame, near, bounds, 1, candidates);
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
    ASSERT_TRUE(runtime->Prepare(view, 1, 0, frame));
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

    ASSERT_TRUE(runtime->Prepare(view, 2, 1, frame));
    std::vector<ForestNearMeshCandidate> candidates;
    const ForestNearMeshView near{view, 2.0f, 0.0f, 0.01f};
    CollectForestNearMeshCandidates(*runtime, frame, near, {0, 1, 0, 2}, 1, candidates);
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
    CollectForestNearMeshCandidates(*runtime, frame, near, {0, 1, 0, 2}, 3, all);
    const auto raised = std::ranges::find_if(all, [&second](const ForestNearMeshCandidate& tree)
                                             { return tree.Ticket == second && tree.LocalPlacement == 0; });
    ASSERT_NE(raised, all.end());
    EXPECT_DOUBLE_EQ(raised->Root.Y, 8.23);
    EXPECT_NEAR(secondRecords[0].YawRadians(), 255.0 * 6.283185307179586 / 256.0, 1e-12);

    auto turned = near;
    turned.PlacementView.EyeRelativePlanes[0] = {1, 0, 0, -40};
    ASSERT_TRUE(runtime->Prepare(turned.PlacementView, 3, 2, frame));
    EXPECT_TRUE(runtime->ReadyPlacements(first).empty());
    CollectForestNearMeshCandidates(*runtime, frame, turned, {0, 1, 0, 2}, 4, candidates);
    ASSERT_EQ(candidates.size(), 2u);
    EXPECT_EQ(candidates.front().Ticket, second);

    runtime->ResetResidency();
    EXPECT_TRUE(runtime->ReadyPlacements(second).empty());
    CollectForestNearMeshCandidates(*runtime, frame, turned, {0, 1, 0, 2}, 4, candidates);
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
    if (!runtime || !runtime->Prepare(view, 1, 0, frame) || frame.Reads.size() != 1u)
        return nullptr;
    const auto ticket = frame.Reads.front();
    std::array<uint8, sizeof(PackedPlacement)> bytes{};
    std::memcpy(bytes.data(), &tree, sizeof(tree));
    if (!runtime->CompleteRead(ticket, bytes) || !runtime->CompleteUpload(ticket, ForestUploadResult::Published) ||
        !runtime->Prepare(view, 2, 1, frame))
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
    CollectForestNearMeshCandidates(*runtime, frame, {view, 2.0f, 0.0f, 0.0f}, {2, 1, 0, 2}, 1, candidates);
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
    CollectForestNearMeshCandidates(*runtime, frame, {view, 2.0f, 0.0f, kTreeCoverage * 0.99f}, {0, 0, 0, 1}, 1,
                                    candidates, std::span(&owner, 1u));
    EXPECT_EQ(candidates.size(), 1u);
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
    ASSERT_TRUE(BuildForestPlacementGpuInputs(*runtime, plan, camera, 1, 15.0f, {0, 7, 0, 8}, gpu, pages, claims));
    ASSERT_EQ(pages.size(), 1u);
    constexpr std::array<uint32, 8> mask{0x80000001u, 1u, 0u, 0x80000000u, 0x8001u, 0u, 0u, 0u};
    EXPECT_EQ(pages.front().MeshOwned, mask);

    ASSERT_TRUE(BuildForestPlacementGpuInputs(*runtime, plan, camera, 1, 15.0f, {0, 7, 0, 8}, gpu, pages));
    ASSERT_EQ(pages.size(), 1u);
    EXPECT_EQ(pages.front().MeshOwned, (std::array<uint32, 8>{}));
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
    ASSERT_TRUE(runtime->Prepare(visible, 1, 0, plan));
    ASSERT_EQ(plan.Reads.size(), 1u);
    const auto ticket = plan.Reads.front();
    ASSERT_EQ(ticket.Page, 1u);
    ASSERT_TRUE(runtime->CompleteRead(ticket, kSecondPage));
    ASSERT_TRUE(runtime->CompleteUpload(ticket, ForestUploadResult::Published));
    ASSERT_TRUE(runtime->Prepare(visible, 2, 1, plan));

    Rendering::CameraData camera{};
    camera.view = MakeLookAtLH({}, {0, 0, 1}, {0, 1, 0});
    camera.proj = MakeOrthographicLH_ZO_ReverseZ(-30, 30, -25, 30, 0.1f, 80.0f);
    camera.viewProj = camera.proj * camera.view;
    SetCameraEye(camera, position);
    ForestPlacementGpuView gpu{};
    std::vector<ForestPlacementGpuPage> pages;
    ASSERT_TRUE(BuildForestPlacementGpuInputs(*runtime, plan, camera, 1, 15.0f, {0, 7, 0, 8}, gpu, pages));
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
    ASSERT_TRUE(BuildForestPlacementGpuInputs(*runtime, missing, camera, 1, 15.0f, {0, 7, 0, 8}, gpu, pages));
    EXPECT_EQ(pages[0].Range[2], 0u);

    camera.proj = MakeOrthographicLH_ZO_ReverseZ(-1, 1.5f, -25, 30, 0.1f, 80.0f);
    camera.viewProj = camera.proj * camera.view;
    SetCameraEye(camera, position);
    ASSERT_TRUE(BuildForestPlacementGpuInputs(*runtime, plan, camera, 1, 15.0f, {0, 7, 0, 8}, gpu, pages));
    ASSERT_EQ(pages.size(), 1u);
    EXPECT_EQ(pages[0].Range[2], 0u); // A verified root lies outside the right edge.

    // Page 1's roots stand 0 and 0.23 meters above its origin, at the eye's height.
    camera.proj = MakeOrthographicLH_ZO_ReverseZ(-30, 30, -25, 0.3f, 0.1f, 80.0f);
    camera.viewProj = camera.proj * camera.view;
    SetCameraEye(camera, position);
    ASSERT_TRUE(BuildForestPlacementGpuInputs(*runtime, plan, camera, 1, 15.0f, {0, 7, 0, 8}, gpu, pages));
    EXPECT_EQ(pages[0].Range[2], 1u);
    camera.proj = MakeOrthographicLH_ZO_ReverseZ(-30, 30, -25, 0.1f, 0.1f, 80.0f);
    camera.viewProj = camera.proj * camera.view;
    SetCameraEye(camera, position);
    ASSERT_TRUE(BuildForestPlacementGpuInputs(*runtime, plan, camera, 1, 15.0f, {0, 7, 0, 8}, gpu, pages));
    EXPECT_EQ(pages[0].Range[2], 0u); // The higher root lies above the top edge.

    ++plan.DetailedDraws.front().FirstPlacement;
    EXPECT_FALSE(BuildForestPlacementGpuInputs(*runtime, plan, camera, 1, 15.0f, {0, 7, 0, 8}, gpu, pages));
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

    // True when the runtime still asks for the near page: no stream has read it.
    bool NoPageWasRead()
    {
        Engine::Renderer::ForestPlacementFrame plan;
        return m_Runtime->Prepare(m_Near, 100, 0, plan) && plan.Reads.size() == 1;
    }

    static constexpr uint64 kWorld = 7;
    Rendering::Tests::ScopedEnvVar m_Headless{"GE_HEADLESS_TEST", "1"};
    const Engine::Renderer::ForestPlacementView m_Near{{512'000'054.0, 8.0, 511'999'990.0}, {}, 100.0f};
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

} // namespace
} // namespace GameEngine::PageStreaming
