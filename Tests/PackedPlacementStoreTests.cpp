#include "PageStreaming/Placement/PackedPlacementStore.h"

#include "AssetCore/SharedFileRead.h"
#include "Assets/AssetManager.h"
#include "Engine/Build/ForestPlacementExport.h"
#include "Engine/Build/GameConfig.h"
#include "Engine/Build/ShipRules.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "Engine/Rendering/Forest/ForestNearMeshCandidates.h"
#include "Engine/Rendering/Impostors/ImpostorStage.h"
#include "Rendering/Core/GPUScene.h"
#include "Engine/Rendering/Forest/ForestPlacementGpuData.h"
#include "Engine/Rendering/Forest/ForestPlacementGpuUploads.h"
#include "Engine/Rendering/Forest/ForestPlacementRuntime.h"
#include "Engine/Rendering/Forest/ForestPlacementStream.h"
#include "Engine/Rendering/Forest/ForestStandDraws.h"
#include "Engine/Rendering/RenderOrigin.h"
#include "Engine/Rendering/RenderServices.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Mathematics/MatrixOps.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "TestTempDir.h"
#include "Types/Fnv1a.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>

namespace GameEngine::PageStreaming
{
namespace
{

constexpr uint64 kHeaderBytes = 32;
constexpr uint64 kDirectoryBytes = 2 * sizeof(PlacementPage);
constexpr uint64 kDataOffset = kHeaderBytes + kDirectoryBytes;
constexpr std::array<uint8, 8> kFirstPage{12, 0, 99, 0, 0, 0, 0, 128};
constexpr std::array<uint8, 16> kSecondPage{
    16, 0, 30, 0, 23, 0, 255, 0,
    64, 1, 88, 2, 255, 255, 255, 255,
};

template<class T>
void Write(std::ofstream& out, const T& value)
{
    out.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

class PackedPlacementStoreTest : public ::testing::Test
{
protected:
    TestUtils::ScopedTempDir m_Scratch{TestUtils::MakeUniqueTempDirectory("PackedPlacementStore")};
    std::array<PlacementPage, 2> m_Pages{
        PlacementPage{{512'000'000.0, 8.0, 512'000'001.0}, {-2, 0, -2}, {18, 25, 18},
                      Hashing::Fnv1a64(kFirstPage.data(), kFirstPage.size()), 0, 1},
        PlacementPage{{512'000'055.0, 8.0, 512'000'002.0}, {-2, 0, -2}, {18, 25, 18},
                      Hashing::Fnv1a64(kSecondPage.data(), kSecondPage.size()), 1, 2},
    };

    void WriteStore(const std::filesystem::path& path, bool corruptDirectory = false)
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out);
        Write(out, std::array<char, 4>{'G', 'E', 'F', 'P'});
        Write(out, uint32{1});
        Write(out, uint32{2});
        Write(out, uint32{3});
        Write(out, kDataOffset);
        const uint64 hash = Hashing::Fnv1a64(m_Pages.data(), kDirectoryBytes);
        Write(out, corruptDirectory ? hash ^ 1 : hash);
        Write(out, m_Pages);
        Write(out, kFirstPage);
        Write(out, kSecondPage);
        out.close();
        ASSERT_TRUE(out);
    }
};

TEST_F(PackedPlacementStoreTest, StagesConfiguredArchiveAtThePlayersAssetPath)
{
    const auto source = m_Scratch.Path() / "Project" / "Assets";
    const auto staging = m_Scratch.Path() / "Package" / "Assets";
    const ForestStandConfig forest{"Forests/Valid.geforest", "Canopy", {}, 400.0f};
    std::filesystem::create_directories(source / "Forests");
    WriteStore(source / forest.archive);
    ASSERT_TRUE(StageForestPlacementArchive(forest, source, staging, ShipRules{}).empty());
    PackedPlacementStore archive;
    ASSERT_TRUE(archive.Open(staging / forest.archive).empty());
    EXPECT_EQ(archive.PlacementCount(), 3u);

    std::ofstream(source / "Forests" / ".assetfolder") << R"({"ship":"never"})";
    std::vector<std::string> errors;
    const auto rules = ShipRules::Load(source, errors);
    ASSERT_TRUE(errors.empty());
    const auto denied = m_Scratch.Path() / "Denied";
    EXPECT_NE(StageForestPlacementArchive(forest, source, denied, rules).find("forestStand.archive"), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(denied));

    auto bad = forest;
    bad.archive = "../Outside.geforest";
    WriteStore(source.parent_path() / "Outside.geforest");
    EXPECT_FALSE(StageForestPlacementArchive(bad, source, denied, ShipRules{}).empty());
    bad.archive = "Broken.geforest";
    WriteStore(source / bad.archive, true);
    EXPECT_FALSE(StageForestPlacementArchive(bad, source, denied, ShipRules{}).empty());
    EXPECT_FALSE(std::filesystem::exists(denied));
}

TEST_F(PackedPlacementStoreTest, WritesTheCanonicalArchiveAndKeepsItAfterARejectedCook)
{
    const auto file = m_Scratch.Path() / "written.geforest";
    std::array<PackedPlacement, 3> placements{};
    std::memcpy(placements.data(), kFirstPage.data(), kFirstPage.size());
    std::memcpy(placements.data() + 1, kSecondPage.data(), kSecondPage.size());
    auto pages = m_Pages;
    for (auto& page : pages)
        page.ContentHash = 0;
    ASSERT_TRUE(WritePackedPlacementStore(file, pages, placements).empty());

    PackedPlacementStore store;
    ASSERT_TRUE(store.Open(file).empty());
    EXPECT_TRUE(store.VerifyPage(0, kFirstPage));
    EXPECT_TRUE(store.VerifyPage(1, kSecondPage));
    const auto expected = m_Scratch.Path() / "reference.geforest";
    WriteStore(expected);
    const auto bytes = [](const auto& path)
    {
        std::ifstream input(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(input), {});
    };
    ASSERT_EQ(bytes(file), bytes(expected));

    ++pages[1].First;
    EXPECT_FALSE(WritePackedPlacementStore(file, pages, placements).empty());
    EXPECT_EQ(bytes(file), bytes(expected));
}

TEST_F(PackedPlacementStoreTest, ResolvesVariablePageRangesAndVerifiesPayload)
{
    const auto file = m_Scratch.Path() / "valid.geforest";
    WriteStore(file);
    PackedPlacementStore store;
    ASSERT_TRUE(store.Open(file).empty());
    EXPECT_EQ(store.File(), file);
    EXPECT_EQ(store.PlacementCount(), 3u);
    ASSERT_EQ(store.Pages().size(), 2u);
    EXPECT_EQ(store.Pages()[1].Origin, m_Pages[1].Origin);
    EXPECT_EQ(store.Pages()[1].LocalMax, m_Pages[1].LocalMax);
    const auto first = store.ReadRange(0);
    const auto second = store.ReadRange(1);
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_EQ(first->Offset, kDataOffset);
    EXPECT_EQ(first->Bytes, kFirstPage.size());
    EXPECT_EQ(second->Offset, kDataOffset + kFirstPage.size());
    EXPECT_EQ(second->Bytes, kSecondPage.size());
    EXPECT_FALSE(store.ReadRange(2));
    EXPECT_FALSE(store.VerifyPage(2, {}));
    EXPECT_FALSE(store.VerifyPage(0, kSecondPage));

    SharedFileReader reader;
    ASSERT_TRUE(reader.Open(file));
    ASSERT_TRUE(reader.SeekTo(second->Offset));
    std::array<uint8, kSecondPage.size()> bytes{};
    ASSERT_EQ(reader.Read(bytes.data(), bytes.size()), static_cast<int64>(bytes.size()));
    EXPECT_TRUE(store.VerifyPage(1, bytes));
    bytes[5] ^= 1;
    EXPECT_FALSE(store.VerifyPage(1, bytes));
}

TEST_F(PackedPlacementStoreTest, RejectsMalformedDirectoriesAndClearsPreviousState)
{
    const auto valid = m_Scratch.Path() / "valid.geforest";
    const auto invalid = m_Scratch.Path() / "invalid.geforest";
    WriteStore(valid);
    PackedPlacementStore store;
    const auto expectRejected = [&]
    {
        ASSERT_TRUE(store.Open(valid).empty());
        EXPECT_FALSE(store.Open(invalid).empty());
        EXPECT_TRUE(store.Pages().empty());
        EXPECT_TRUE(store.File().empty());
        EXPECT_EQ(store.PlacementCount(), 0u);
        EXPECT_FALSE(store.ReadRange(0));
    };

    WriteStore(invalid, true);
    expectRejected();
    const auto original = m_Pages[1];
    m_Pages[1].First = 0;
    WriteStore(invalid);
    expectRejected();
    m_Pages[1] = original;
    m_Pages[1].Count = PackedPlacementStore::kPlacementsPerPage + 1;
    WriteStore(invalid);
    expectRejected();
    m_Pages[1] = original;
    m_Pages[1].Origin[2] = std::numeric_limits<double>::infinity();
    WriteStore(invalid);
    expectRejected();
    m_Pages[1] = original;
    m_Pages[1].LocalMin[2] = m_Pages[1].LocalMax[2] + 1;
    WriteStore(invalid);
    expectRejected();
    m_Pages[1] = original;
    WriteStore(invalid);
    std::filesystem::resize_file(invalid, kDataOffset + 1);
    expectRejected();
    ASSERT_TRUE(store.Open(valid).empty());
    EXPECT_EQ(store.PlacementCount(), 3u);
}

TEST_F(PackedPlacementStoreTest, KeepsFallbackThroughUploadAndProtectsRetiringSlotUntilLastDraw)
{
    using Engine::Renderer::ForestPlacementRuntime;
    using Engine::Renderer::ForestPlacementFrame;
    using Engine::Renderer::ForestPlacementView;

    const auto file = m_Scratch.Path() / "valid.geforest";
    WriteStore(file);
    constexpr std::array<uint32, 3> offsets{20, 26, 35};
    std::string error;
    const auto planner = ForestPlacementRuntime::Open(file, offsets, 1, 1, error);
    ASSERT_TRUE(planner) << error;
    ForestPlacementView both{{512'000'000.0, 8.0, 512'000'001.0}, {}, 100.0f};
    EXPECT_TRUE(planner->CanRequestDetails(both));
    auto overview = both;
    overview.Eye.Y += 1000;
    EXPECT_FALSE(planner->CanRequestDetails(overview));
    ForestPlacementFrame frame;
    ASSERT_TRUE(planner->Prepare(both, 1, 0, frame));
    EXPECT_TRUE(frame.WantsDetails);
    ASSERT_EQ(frame.StandDraws.size(), 1u);
    EXPECT_EQ(frame.StandDraws[0].FirstIndex, 20u);
    EXPECT_EQ(frame.StandDraws[0].IndexCount, 15u);
    EXPECT_EQ(frame.VisibleCells, 2u);
    ASSERT_EQ(frame.Reads.size(), 1u);
    const auto first = frame.Reads.front();
    EXPECT_EQ(first.Page, 0u);
    EXPECT_TRUE(planner->CompleteRead(first, kFirstPage));

    ASSERT_TRUE(planner->Prepare(both, 2, 0, frame));
    EXPECT_TRUE(frame.DetailedDraws.empty());
    ASSERT_EQ(frame.StandDraws.size(), 1u);
    EXPECT_EQ(frame.StandDraws[0].IndexCount, 15u);
    ASSERT_TRUE(planner->CompleteUpload(first, true));
    ASSERT_TRUE(planner->Prepare(both, 3, 1, frame));
    ASSERT_EQ(frame.DetailedDraws.size(), 1u);
    EXPECT_EQ(frame.DetailedDraws.front().Ticket, first);
    EXPECT_EQ(frame.DetailedDraws.front().PlacementCount, 1u);
    ASSERT_EQ(frame.StandDraws.size(), 1u);
    EXPECT_EQ(frame.StandDraws[0].FirstIndex, 26u);

    ForestPlacementView secondOnly = both;
    secondOnly.EyeRelativePlanes[0] = {1, 0, 0, -40};
    ASSERT_TRUE(planner->Prepare(secondOnly, 4, 2, frame));
    EXPECT_EQ(frame.VisibleCells, 1u);
    EXPECT_TRUE(frame.Reads.empty());
    EXPECT_TRUE(frame.DetailedDraws.empty());
    ASSERT_TRUE(planner->Prepare(secondOnly, 5, 3, frame));
    ASSERT_EQ(frame.Reads.size(), 1u);
    const auto second = frame.Reads.front();
    EXPECT_EQ(second.Page, 1u);
    EXPECT_EQ(second.Slot, first.Slot);
    EXPECT_NE(second.Generation, first.Generation);
    EXPECT_FALSE(planner->CompleteUpload(first, true));

    // An invalid frame cannot lose the pending read or send it to the slot twice.
    EXPECT_FALSE(planner->Prepare(secondOnly, 5, 3, frame));
    EXPECT_TRUE(frame.Reads.empty());
    EXPECT_TRUE(planner->CompleteRead(second, kSecondPage));
    EXPECT_TRUE(planner->CompleteUpload(second, true));
    ASSERT_TRUE(planner->Prepare(secondOnly, 6, 4, frame));
    EXPECT_TRUE(frame.StandDraws.empty());
    ASSERT_EQ(frame.DetailedDraws.size(), 1u);
    EXPECT_EQ(frame.DetailedDraws.front().PlacementCount, 2u);

    planner->ResetResidency();
    ASSERT_TRUE(planner->Prepare(secondOnly, 1, 0, frame));
    EXPECT_TRUE(frame.WantsDetails);
    EXPECT_TRUE(frame.DetailedDraws.empty());
    ASSERT_EQ(frame.StandDraws.size(), 1u);
    ASSERT_EQ(frame.Reads.size(), 1u);
    EXPECT_EQ(frame.Reads.front().Page, second.Page);
    EXPECT_NE(frame.Reads.front().Generation, second.Generation);
    EXPECT_FALSE(planner->CompleteRead(second, kSecondPage));
}

TEST_F(PackedPlacementStoreTest, ReturningViewReusesAReadyPageAndExtendsItsReaderFence)
{
    using namespace Engine::Renderer;
    const auto file = m_Scratch.Path() / "camera-return.geforest";
    WriteStore(file);
    std::string error;
    auto runtime = ForestPlacementRuntime::OpenQuadStand(file, error, 1, 1, true);
    ASSERT_TRUE(runtime) << error;

    const ForestPlacementView near{{512'000'000.0, 8.0, 512'000'001.0}, {}, 100.0f};
    auto turned = near;
    turned.EyeRelativePlanes[0] = {1, 0, 0, -40};
    ForestPlacementFrame frame;
    ASSERT_TRUE(runtime->Prepare(near, 1, 0, frame));
    ASSERT_EQ(frame.Reads.size(), 1u);
    const auto ticket = frame.Reads.front();
    ASSERT_EQ(ticket.Page, 0u);
    ASSERT_TRUE(runtime->CompleteRead(ticket, kFirstPage));
    ASSERT_TRUE(runtime->CompleteUpload(ticket, true));
    ASSERT_TRUE(runtime->Prepare(near, 2, 1, frame));
    ASSERT_EQ(frame.DetailedDraws.size(), 1u);

    ASSERT_TRUE(runtime->Prepare(turned, 3, 1, frame));
    EXPECT_TRUE(frame.Reads.empty());
    EXPECT_TRUE(frame.DetailedDraws.empty());
    ASSERT_TRUE(runtime->Prepare(near, 4, 1, frame));
    EXPECT_TRUE(frame.Reads.empty());
    ASSERT_EQ(frame.DetailedDraws.size(), 1u);
    EXPECT_EQ(frame.DetailedDraws.front().Ticket, ticket);
    EXPECT_EQ(runtime->ReadyPlacements(ticket).size(), 1u);

    auto overview = near;
    overview.DetailDistance = 0.0f;
    for (uint64 index = 5; index <= 9; ++index)
    {
        ASSERT_TRUE(runtime->Prepare(overview, index, index - 1, frame));
        EXPECT_TRUE(frame.DetailedDraws.empty());
        EXPECT_TRUE(frame.Reads.empty());
        ASSERT_EQ(frame.StandDraws.size(), 1u);
        EXPECT_EQ(frame.StandDraws.front().IndexCount, 12u);
    }
    // The old reader fence completed while the page was offscreen.
    ASSERT_TRUE(runtime->Prepare(near, 10, 9, frame));
    EXPECT_TRUE(frame.Reads.empty());
    ASSERT_EQ(frame.DetailedDraws.size(), 1u);
    EXPECT_EQ(frame.DetailedDraws.front().Ticket, ticket);

    ASSERT_TRUE(runtime->Prepare(turned, 11, 9, frame));
    EXPECT_TRUE(frame.DetailedDraws.empty());
    EXPECT_TRUE(frame.Reads.empty());
    ASSERT_TRUE(runtime->Prepare(turned, 12, 10, frame));
    ASSERT_EQ(frame.Reads.size(), 1u);
    EXPECT_EQ(frame.Reads.front().Slot, ticket.Slot);
    EXPECT_NE(frame.Reads.front().Generation, ticket.Generation);
    EXPECT_EQ(frame.Reads.front().Page, 1u);
    EXPECT_TRUE(runtime->ReadyPlacements(ticket).empty());
}

TEST_F(PackedPlacementStoreTest, NearMeshTransformsRetainPackedSectorYawAndScale)
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

TEST_F(PackedPlacementStoreTest, CardEntryRespectsTheCoarsestLevelOfEveryPart)
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

TEST_F(PackedPlacementStoreTest, DeformationWidensTheNearMeshFrustumWithoutMovingItsLodSwitch)
{
    using namespace Engine::Renderer;
    const auto file = m_Scratch.Path() / "wind-frustum.geforest";
    WriteStore(file);
    std::string error;
    auto runtime = ForestPlacementRuntime::OpenQuadStand(file, error, 1, 1, true);
    ASSERT_TRUE(runtime) << error;
    const ForestPlacementView view{{512'000'000.0, 8.0, 512'000'001.0}, {}, 100.0f};
    ForestPlacementFrame frame;
    ASSERT_TRUE(runtime->Prepare(view, 1, 0, frame));
    ASSERT_EQ(frame.Reads.size(), 1u);
    const auto ticket = frame.Reads.front();
    ASSERT_EQ(ticket.Page, 0u);
    ASSERT_TRUE(runtime->CompleteRead(ticket, kFirstPage));
    ASSERT_TRUE(runtime->CompleteUpload(ticket, true));

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

TEST_F(PackedPlacementStoreTest, NearMeshHysteresisKeepsOnlyTheSamePlacementGeneration)
{
    using namespace Engine::Renderer;
    const auto file = m_Scratch.Path() / "hysteresis.geforest";
    WriteStore(file);
    std::string error;
    auto runtime = ForestPlacementRuntime::OpenQuadStand(file, error, 1, 1, true);
    ASSERT_TRUE(runtime) << error;
    const ForestPlacementView view{{512'000'000.0, 8.0, 512'000'001.0}, {}, 100.0f};
    ForestPlacementFrame frame;
    ASSERT_TRUE(runtime->Prepare(view, 1, 0, frame));
    ASSERT_EQ(frame.Reads.size(), 1u);
    const auto ticket = frame.Reads.front();
    ASSERT_EQ(ticket.Page, 0u);
    ASSERT_TRUE(runtime->CompleteRead(ticket, kFirstPage));
    ASSERT_TRUE(runtime->CompleteUpload(ticket, true));
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

TEST_F(PackedPlacementStoreTest, NearMeshesUseOnlyVerifiedAndPublishedPlacementTransforms)
{
    using namespace Engine::Renderer;
    const auto file = m_Scratch.Path() / "near-mesh.geforest";
    WriteStore(file);
    std::string error;
    auto runtime = ForestPlacementRuntime::OpenQuadStand(file, error, 2, 2, true);
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
    ASSERT_TRUE(runtime->CompleteUpload(first, true));
    ASSERT_TRUE(runtime->CompleteUpload(second, true));

    ASSERT_TRUE(runtime->Prepare(view, 2, 1, frame));
    std::vector<ForestNearMeshCandidate> candidates;
    const ForestNearMeshView near{view, 2.0f, 0.0f, 0.01f};
    CollectForestNearMeshCandidates(*runtime, frame, near, {0, 1, 0, 2}, 1, candidates);
    ASSERT_EQ(candidates.size(), 1u);
    const auto& placement = candidates.front();
    EXPECT_EQ(placement.Ticket, first);
    EXPECT_EQ(placement.LocalPlacement, 0u);
    EXPECT_DOUBLE_EQ(placement.Root.X, 512'000'000.12);
    EXPECT_DOUBLE_EQ(placement.Root.Z, 512'000'001.99);
    EXPECT_FLOAT_EQ(placement.Scale, static_cast<float>(0.75 + 0.5 * 32768.0 / 65535.0));
    const auto secondRecords = runtime->ReadyPlacements(second);
    ASSERT_EQ(secondRecords.size(), 2u);
    EXPECT_NEAR(secondRecords[0].YawRadians(), 23.0 * 6.283185307179586 / 65536.0, 1e-12);

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

TEST_F(PackedPlacementStoreTest, AllowsStandOnlyAndRejectsInvalidIndexOffsets)
{
    using Engine::Renderer::ForestPlacementRuntime;
    using Engine::Renderer::ForestPlacementFrame;

    const auto file = m_Scratch.Path() / "valid.geforest";
    WriteStore(file);
    std::string error;
    constexpr std::array<uint32, 3> invalid{20, 19, 30};
    EXPECT_FALSE(ForestPlacementRuntime::Open(file, invalid, 0, 4, error));
    EXPECT_FALSE(error.empty());

    constexpr std::array<uint32, 3> offsets{20, 26, 35};
    const auto planner = ForestPlacementRuntime::Open(file, offsets, 0, 4, error);
    ASSERT_TRUE(planner) << error;
    ForestPlacementFrame frame;
    ASSERT_TRUE(planner->Prepare({{512'000'000.0, 8.0, 512'000'001.0}, {}, 100.0f}, 1, 0, frame));
    EXPECT_FALSE(frame.WantsDetails);
    ASSERT_EQ(frame.StandDraws.size(), 1u);
    EXPECT_EQ(frame.StandDraws[0].FirstIndex, 20u);
    EXPECT_EQ(frame.StandDraws[0].IndexCount, 15u);
    EXPECT_TRUE(frame.Reads.empty());
    EXPECT_TRUE(frame.DetailedDraws.empty());
    EXPECT_FALSE(planner->CompleteRead({0, 0, 0}, {}));
    EXPECT_FALSE(planner->CompleteUpload({0, 0, 0}, true));
}

TEST_F(PackedPlacementStoreTest, ColdDetailDemandRequiresAVisiblePageWithinLookahead)
{
    using namespace Engine::Renderer;
    const auto file = m_Scratch.Path() / "cold.geforest";
    WriteStore(file);
    std::string error;
    auto runtime = ForestPlacementRuntime::OpenQuadStand(file, error, 2, 2);
    ASSERT_TRUE(runtime) << error;

    ForestPlacementView view{{512'000'032.0, 8.0, 512'000'001.0}, {}, 10.0f};
    // The root covers the middle of the gap. Neither page is within range.
    EXPECT_FALSE(runtime->CanRequestDetails(view));
    EXPECT_FALSE(runtime->CanPrefetchDetails(view));
    ForestPlacementFrame plan;
    ASSERT_TRUE(runtime->Prepare(view, 1, 0, plan));
    EXPECT_FALSE(plan.WantsDetails);
    EXPECT_TRUE(plan.Reads.empty());
    ASSERT_EQ(plan.StandDraws.size(), 1u);
    EXPECT_EQ(plan.StandDraws.front().IndexCount, 12u);
    view.DetailDistance = 11.0f;
    EXPECT_TRUE(runtime->CanPrefetchDetails(view));
    EXPECT_FALSE(runtime->CanRequestDetails(view));
    view.EyeRelativePlanes[0] = {1, 0, 0, -15};
    EXPECT_FALSE(runtime->CanPrefetchDetails(view));
    view.EyeRelativePlanes = {};
    view.DetailDistance = 25.0f;
    EXPECT_TRUE(runtime->CanRequestDetails(view));
    view.EyeRelativePlanes[0] = {1, 0, 0, -15};
    EXPECT_TRUE(runtime->CanRequestDetails(view)); // Only the second page remains visible.
    view.DetailDistance = 15.0f;
    EXPECT_FALSE(runtime->CanRequestDetails(view)); // The near page is outside the frustum.
    view.EyeRelativePlanes[0] = {1, 0, 0, -100};
    view.DetailDistance = 100.0f;
    EXPECT_FALSE(runtime->CanRequestDetails(view));
}

TEST_F(PackedPlacementStoreTest, PrefetchKeepsTheStandUntilThePageEntersTheDetailRange)
{
    using namespace Engine::Renderer;
    const auto file = m_Scratch.Path() / "detail-lookahead.geforest";
    WriteStore(file);
    std::string error;
    auto runtime = ForestPlacementRuntime::OpenQuadStand(file, error, 1, 1);
    ASSERT_TRUE(runtime) << error;

    ForestPlacementView view{{512'000'000.0 - 34.0, 8.0, 512'000'001.0}, {}, 25.0f};
    ASSERT_FALSE(runtime->CanRequestDetails(view));
    ASSERT_TRUE(runtime->CanPrefetchDetails(view));
    auto wideView = view;
    wideView.DetailDistance = std::numeric_limits<float>::max();
    EXPECT_TRUE(runtime->CanPrefetchDetails(wideView));
    ForestPlacementFrame frame;
    ASSERT_TRUE(runtime->Prepare(view, 1, 0, frame));
    ASSERT_EQ(frame.Reads.size(), 1u);
    const auto ticket = frame.Reads.front();
    ASSERT_EQ(ticket.Page, 0u);
    EXPECT_TRUE(frame.DetailedDraws.empty());
    ASSERT_EQ(frame.StandDraws.size(), 1u);
    EXPECT_EQ(frame.StandDraws.front().IndexCount, 12u);
    ASSERT_TRUE(runtime->CompleteRead(ticket, kFirstPage));
    ASSERT_TRUE(runtime->CompleteUpload(ticket, true));

    ASSERT_TRUE(runtime->Prepare(view, 2, 1, frame));
    EXPECT_TRUE(frame.Reads.empty());
    EXPECT_TRUE(frame.DetailedDraws.empty());
    ASSERT_EQ(frame.StandDraws.size(), 1u);
    EXPECT_EQ(frame.StandDraws.front().IndexCount, 12u);

    view.Eye.X = 512'000'000.0;
    ASSERT_TRUE(runtime->Prepare(view, 3, 2, frame));
    EXPECT_TRUE(frame.Reads.empty());
    ASSERT_EQ(frame.DetailedDraws.size(), 1u);
    EXPECT_EQ(frame.DetailedDraws.front().Ticket, ticket);
    ASSERT_EQ(frame.StandDraws.size(), 1u);
    EXPECT_EQ(frame.StandDraws.front().FirstIndex, 6u);
    EXPECT_EQ(frame.StandDraws.front().IndexCount, 6u);
}

TEST_F(PackedPlacementStoreTest, TransitionSlotStartsDemandWhilePreviousDetailDrawIsInFlight)
{
    using namespace Engine::Renderer;
    const auto file = m_Scratch.Path() / "transition.geforest";
    WriteStore(file);
    std::string error;
    auto runtime = ForestPlacementRuntime::OpenQuadStand(file, error, 1, 1, false, 1);
    ASSERT_TRUE(runtime) << error;
    ASSERT_EQ(runtime->SlotCount(), 2u);

    ForestPlacementView view{{512'000'000.0, 8.0, 512'000'001.0}, {}, 25.0f};
    ForestPlacementFrame frame;
    ASSERT_TRUE(runtime->Prepare(view, 1, 0, frame));
    ASSERT_EQ(frame.Reads.size(), 1u);
    const auto first = frame.Reads.front();
    ASSERT_EQ(first.Page, 0u);
    ASSERT_TRUE(runtime->CompleteRead(first, kFirstPage));
    ASSERT_TRUE(runtime->CompleteUpload(first, true));

    ASSERT_TRUE(runtime->Prepare(view, 2, 1, frame));
    ASSERT_EQ(frame.DetailedDraws.size(), 1u);
    EXPECT_EQ(frame.DetailedDraws.front().Ticket, first);

    view.Eye.X = 512'000'104.0;
    ASSERT_TRUE(runtime->CanPrefetchDetails(view));
    ASSERT_FALSE(runtime->CanRequestDetails(view));
    ASSERT_TRUE(runtime->Prepare(view, 3, 1, frame));
    EXPECT_TRUE(frame.Reads.empty()); // This request cannot occupy the transition slot.
    EXPECT_TRUE(frame.DetailedDraws.empty());

    view.Eye.X = 512'000'055.0;
    ASSERT_TRUE(runtime->Prepare(view, 4, 1, frame));
    ASSERT_EQ(frame.Reads.size(), 1u);
    const auto second = frame.Reads.front();
    EXPECT_EQ(second.Page, 1u);
    EXPECT_NE(second.Slot, first.Slot);
    EXPECT_TRUE(frame.DetailedDraws.empty());
    ASSERT_EQ(frame.StandDraws.size(), 1u);
    EXPECT_EQ(frame.StandDraws.front().IndexCount, 12u);
    ASSERT_TRUE(runtime->CompleteRead(second, kSecondPage));
    ASSERT_TRUE(runtime->CompleteUpload(second, true));

    ASSERT_TRUE(runtime->Prepare(view, 5, 1, frame));
    ASSERT_EQ(frame.DetailedDraws.size(), 1u);
    EXPECT_EQ(frame.DetailedDraws.front().Ticket, second);
    ASSERT_EQ(frame.StandDraws.size(), 1u);
    EXPECT_EQ(frame.StandDraws.front().IndexCount, 6u);
}

TEST_F(PackedPlacementStoreTest, EmitsStandRangesAndMatchingDepthOnlyIntoEligibleViews)
{
    using namespace Engine::Renderer;
    using namespace Mathematics;

    const auto file = m_Scratch.Path() / "valid.geforest";
    WriteStore(file);
    constexpr std::array<uint32, 3> offsets{20, 26, 35};
    std::string error;
    std::shared_ptr<ForestPlacementRuntime> planner = ForestPlacementRuntime::Open(file, offsets, 0, 0, error);
    ASSERT_TRUE(planner) << error;

    RenderServices services;
    constexpr uint64 kForestWorld = 45;
    const auto makeView = [&](double x, uint64 world, Rendering::RenderLayerMask layers)
    {
        Rendering::CameraData camera{};
        camera.view = MakeLookAtLH({}, {0, 0, 1}, {0, 1, 0});
        camera.proj = MakeOrthographicLH_ZO_ReverseZ(-25, 25, -25, 30, 0.1f, 80.0f);
        camera.viewProj = camera.proj * camera.view;
        SetCameraEye(camera, {x, 8.0, 511'999'990.0});
        const auto cam = services.Views().AllocateCamera("ForestStandCamera");
        services.Views().SetCameraData(cam, camera, {});
        const auto view = services.Views().AllocateView("ForestStandView", cam);
        services.Views().SetViewWorldId(view, world);
        services.Views().SetViewRenderLayerMask(view, layers);
        return view;
    };
    const auto defaultLayer = Rendering::RenderLayerMask::Default();
    const auto first = makeView(512'000'000.0, kForestWorld, defaultLayer);
    const auto second = makeView(512'000'055.0, kForestWorld, defaultLayer);
    const auto missed = makeView(512'000'500.0, kForestWorld, defaultLayer);
    const auto otherWorld = makeView(512'000'000.0, kForestWorld + 1, defaultLayer);
    const auto hiddenLayer = makeView(512'000'000.0, kForestWorld, Rendering::RenderLayerMask::FromLayer(2));
    unsigned prepared = 0;
    const auto subscription = RegisterForestStandDraws(services, kForestWorld, defaultLayer, planner,
        [&](const ForwardEmitContext& ctx) -> std::optional<ForestStandDrawHeads>
        {
            ++prepared;
            ForestStandDrawHeads heads{};
            heads.Color.FirstIndex = 512;
            heads.Color.VertexOffset = 19;
            heads.Color.IndexCount = 35;
            if (ctx.ViewId == first)
                heads.Depth = heads.Color;
            return heads;
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
    EXPECT_EQ(firstDraws.front().FirstIndex, 532u);
    EXPECT_EQ(firstDraws.front().VertexOffset, 19);
    EXPECT_EQ(firstDraws.front().IndexCount, 6u);
    EXPECT_EQ(secondDraws.front().FirstIndex, 538u);
    EXPECT_EQ(secondDraws.front().VertexOffset, 19);
    EXPECT_EQ(secondDraws.front().IndexCount, 9u);
    EXPECT_EQ(depthDraws.data(), firstDraws.data());
    EXPECT_EQ(depthDraws.size(), firstDraws.size());
    EXPECT_EQ(prepared, 2u);
    for (const auto view : {missed, otherWorld, hiddenLayer})
        EXPECT_TRUE(services.Views().PerView(view).ForwardCommands.empty());
}

TEST_F(PackedPlacementStoreTest, ACameraCutReusesAReadingSlotBeforeItsIoCallback)
{
    using namespace Engine::Renderer;
    const auto file = m_Scratch.Path() / "cut.geforest";
    WriteStore(file);
    std::string error;
    auto planner = ForestPlacementRuntime::OpenQuadStand(file, error, 1, 1);
    ASSERT_TRUE(planner) << error;

    ForestPlacementView view{{512'000'000.0, 8.0, 512'000'001.0}, {}, 100.0f};
    ForestPlacementFrame plan;
    ASSERT_TRUE(planner->Prepare(view, 1, 0, plan));
    ASSERT_EQ(plan.Reads.size(), 1u);
    const auto oldRead = plan.Reads.front();
    ASSERT_EQ(oldRead.Page, 0u);

    view.EyeRelativePlanes[0] = {1, 0, 0, -40};
    ASSERT_TRUE(planner->Prepare(view, 2, 0, plan));
    ASSERT_EQ(plan.CancelReads.size(), 1u);
    EXPECT_EQ(plan.CancelReads.front(), oldRead);
    ASSERT_EQ(plan.Reads.size(), 1u);
    const auto newRead = plan.Reads.front();
    EXPECT_EQ(newRead.Page, 1u);
    EXPECT_EQ(newRead.Slot, oldRead.Slot);
    EXPECT_NE(newRead.Generation, oldRead.Generation);
    EXPECT_TRUE(plan.DetailedDraws.empty());
    ASSERT_EQ(plan.StandDraws.size(), 1u);

    EXPECT_FALSE(planner->CompleteRead(oldRead, kFirstPage));
    EXPECT_TRUE(planner->CompleteRead(newRead, kSecondPage));
    EXPECT_TRUE(planner->CompleteUpload(newRead, true));
    ASSERT_TRUE(planner->Prepare(view, 3, 2, plan));
    ASSERT_EQ(plan.DetailedDraws.size(), 1u);
    EXPECT_EQ(plan.DetailedDraws.front().Ticket, newRead);
    EXPECT_TRUE(plan.StandDraws.empty());
}

TEST_F(PackedPlacementStoreTest, StreamRejectsArrivedBytesWhenTheCameraRetiresThePage)
{
    using namespace Engine::Renderer;
    const auto file = m_Scratch.Path() / "valid.geforest";
    WriteStore(file);
    constexpr std::array<uint32, 3> offsets{0, 6, 12};
    std::string error;
    std::shared_ptr<ForestPlacementRuntime> planner = ForestPlacementRuntime::Open(file, offsets, 1, 1, error);
    ASSERT_TRUE(planner) << error;
    JobSystem::WorkStealingThreadPool jobs(0);
    AssetManager assets;
    ASSERT_TRUE(assets.Initialize(&jobs));
    ForestPlacementStream stream(planner);
    ForestPlacementFrame plan;
    std::vector<ForestPlacementUpload> uploads;
    ForestPlacementView view{{512'000'000.0, 8.0, 512'000'001.0}, {}, 100.0f};

    ASSERT_TRUE(stream.Prepare(assets, view, 1, 0, plan, uploads));
    ASSERT_EQ(plan.Reads.size(), 1u);
    const auto first = plan.Reads.front();
    ASSERT_EQ(first.Page, 0u);
    ASSERT_TRUE(uploads.empty()); // In inline IO, its bytes already arrived.
    view.EyeRelativePlanes[0] = {1, 0, 0, -40};
    ASSERT_TRUE(stream.Prepare(assets, view, 2, 0, plan, uploads));
    EXPECT_TRUE(uploads.empty());
    ASSERT_EQ(plan.CancelReads.size(), 1u);
    EXPECT_EQ(plan.CancelReads.front(), first);
    ASSERT_EQ(plan.StandDraws.size(), 1u);
    EXPECT_EQ(plan.StandDraws.front().FirstIndex, 6u);
    ASSERT_EQ(plan.Reads.size(), 1u);
    const auto second = plan.Reads.front();
    EXPECT_EQ(second.Page, 1u);
    EXPECT_EQ(second.Slot, first.Slot);
    EXPECT_NE(second.Generation, first.Generation);
    ASSERT_TRUE(stream.Prepare(assets, view, 3, 1, plan, uploads));
    EXPECT_TRUE(plan.Reads.empty());
    ASSERT_EQ(uploads.size(), 1u);
    EXPECT_EQ(uploads.front().Ticket, second);
    const auto& bytes = uploads.front().Bytes;
    EXPECT_TRUE(std::ranges::equal(bytes, kSecondPage));
    EXPECT_TRUE(plan.DetailedDraws.empty());
    ASSERT_TRUE(planner->CompleteUpload(second, true));
    ASSERT_TRUE(stream.Prepare(assets, view, 4, 2, plan, uploads));
    EXPECT_TRUE(uploads.empty());
    ASSERT_EQ(plan.DetailedDraws.size(), 1u);
    EXPECT_EQ(plan.DetailedDraws.front().Ticket, second);
    EXPECT_TRUE(plan.StandDraws.empty());
}

TEST_F(PackedPlacementStoreTest, AdjacentPagesShareAReadWhileKeepingIndependentUploadTickets)
{
    using namespace Engine::Renderer;
    const auto file = m_Scratch.Path() / "adjacent.geforest";
    WriteStore(file);
    std::string error;
    auto planner = ForestPlacementRuntime::OpenQuadStand(file, error, 2, 2);
    ASSERT_TRUE(planner) << error;
    JobSystem::WorkStealingThreadPool jobs(0);
    AssetManager assets;
    ASSERT_TRUE(assets.Initialize(&jobs));
    ForestPlacementStream stream(planner);
    ForestPlacementFrame plan;
    std::vector<ForestPlacementUpload> uploads;
    const ForestPlacementView view{{512'000'055.0, 8.0, 512'000'002.0}, {}, 100.0f};

    ASSERT_TRUE(stream.Prepare(assets, view, 1, 0, plan, uploads));
    ASSERT_EQ(plan.Reads.size(), 2u);
    EXPECT_EQ(plan.Reads.front().Page, 1u);
    ASSERT_TRUE(stream.Prepare(assets, view, 2, 0, plan, uploads));
    ASSERT_EQ(uploads.size(), 2u);
    EXPECT_EQ(uploads[0].Ticket.Page, 0u);
    EXPECT_EQ(uploads[1].Ticket.Page, 1u);
    EXPECT_EQ(uploads[0].Bytes.data() + uploads[0].Bytes.size(), uploads[1].Bytes.data());
    EXPECT_TRUE(std::ranges::equal(uploads[0].Bytes, kFirstPage));
    EXPECT_TRUE(std::ranges::equal(uploads[1].Bytes, kSecondPage));
}

TEST_F(PackedPlacementStoreTest, ACoalescedReadKeepsTheOtherPageWhenOneTicketIsRetired)
{
    using namespace Engine::Renderer;
    const auto file = m_Scratch.Path() / "partial.geforest";
    WriteStore(file);
    std::string error;
    auto planner = ForestPlacementRuntime::OpenQuadStand(file, error, 2, 2);
    ASSERT_TRUE(planner) << error;
    JobSystem::WorkStealingThreadPool jobs(0);
    AssetManager assets;
    ASSERT_TRUE(assets.Initialize(&jobs));
    ForestPlacementStream stream(planner);
    ForestPlacementFrame plan;
    std::vector<ForestPlacementUpload> uploads;
    ForestPlacementView view{{512'000'000.0, 8.0, 512'000'001.0}, {}, 100.0f};

    ASSERT_TRUE(stream.Prepare(assets, view, 1, 0, plan, uploads));
    ASSERT_EQ(plan.Reads.size(), 2u);
    const auto retired = plan.Reads.front();
    ASSERT_EQ(retired.Page, 0u);
    view.EyeRelativePlanes[0] = {1, 0, 0, -40};
    ASSERT_TRUE(stream.Prepare(assets, view, 2, 0, plan, uploads));
    ASSERT_EQ(plan.CancelReads.size(), 1u);
    EXPECT_EQ(plan.CancelReads.front(), retired);
    ASSERT_EQ(uploads.size(), 1u);
    EXPECT_EQ(uploads.front().Ticket.Page, 1u);
    EXPECT_TRUE(std::ranges::equal(uploads.front().Bytes, kSecondPage));
    EXPECT_FALSE(planner->CanRecordUpload(retired));
}

TEST_F(PackedPlacementStoreTest, ACoalescedReadVerifiesEveryPageChecksumIndependently)
{
    using namespace Engine::Renderer;
    const auto file = m_Scratch.Path() / "corrupt-page.geforest";
    WriteStore(file);
    {
        std::fstream bytes(file, std::ios::in | std::ios::out | std::ios::binary);
        bytes.seekp(kDataOffset);
        bytes.put(0x13);
        ASSERT_TRUE(bytes);
    }
    std::string error;
    auto planner = ForestPlacementRuntime::OpenQuadStand(file, error, 2, 2);
    ASSERT_TRUE(planner) << error;
    JobSystem::WorkStealingThreadPool jobs(0);
    AssetManager assets;
    ASSERT_TRUE(assets.Initialize(&jobs));
    ForestPlacementStream stream(planner);
    ForestPlacementFrame plan;
    std::vector<ForestPlacementUpload> uploads;
    const ForestPlacementView view{{512'000'000.0, 8.0, 512'000'001.0}, {}, 100.0f};
    ASSERT_TRUE(stream.Prepare(assets, view, 1, 0, plan, uploads));
    ASSERT_EQ(plan.Reads.size(), 2u);
    ASSERT_TRUE(stream.Prepare(assets, view, 2, 0, plan, uploads));
    EXPECT_TRUE(plan.Reads.empty()); // The corrupt page keeps its retry backoff.
    ASSERT_EQ(uploads.size(), 1u);
    EXPECT_EQ(uploads.front().Ticket.Page, 1u);
    EXPECT_TRUE(std::ranges::equal(uploads.front().Bytes, kSecondPage));
    ASSERT_EQ(plan.StandDraws.size(), 1u);
    EXPECT_EQ(plan.StandDraws.front().IndexCount, 12u);
}

TEST_F(PackedPlacementStoreTest, StreamRetainsTheStandWhenTheAssetLoaderIsUnavailable)
{
    using namespace Engine::Renderer;
    const auto file = m_Scratch.Path() / "valid.geforest";
    WriteStore(file);
    constexpr std::array<uint32, 3> offsets{0, 6, 12};
    std::string error;
    std::shared_ptr<ForestPlacementRuntime> planner = ForestPlacementRuntime::Open(file, offsets, 1, 1, error);
    ASSERT_TRUE(planner) << error;
    AssetManager unavailable;
    ForestPlacementStream stream(planner);
    ForestPlacementFrame plan;
    std::vector<ForestPlacementUpload> uploads;
    const ForestPlacementView view{{512'000'000.0, 8.0, 512'000'001.0}, {}, 100.0f};
    ASSERT_TRUE(stream.Prepare(unavailable, view, 1, 0, plan, uploads));
    ASSERT_EQ(plan.Reads.size(), 1u);
    EXPECT_TRUE(uploads.empty());
    ASSERT_TRUE(stream.Prepare(unavailable, view, 2, 1, plan, uploads));
    EXPECT_TRUE(uploads.empty());
    EXPECT_TRUE(plan.DetailedDraws.empty());
    ASSERT_EQ(plan.StandDraws.size(), 1u);
    EXPECT_EQ(plan.StandDraws.front().IndexCount, 12u);
    ASSERT_TRUE(stream.Prepare(unavailable, view, 3, 2, plan, uploads));
    ASSERT_EQ(plan.Reads.size(), 1u);
    EXPECT_EQ(plan.Reads.front().Page, 1u); // The failed page is in cooldown.
}

TEST_F(PackedPlacementStoreTest, CombinesOnlyByteAdjacentForestCopiesAndPreservesTheStagingOrder)
{
    using namespace Engine::Renderer;
    constexpr size_t slotBytes = PackedPlacementStore::kPlacementsPerPage * sizeof(PackedPlacement);
    const std::array<uint8, slotBytes> full{};
    std::array<ForestPlacementUpload, 5> uploads{{
        {{2, 17, 1}, full}, {{5, 3, 1}, kSecondPage}, {{0, 5, 9}, kFirstPage},
        {{4, 1, 7}, kFirstPage}, {{3, 19, 3}, full}
    }};
    const auto plan = BuildForestPlacementUploadPlan(uploads, 6);
    ASSERT_TRUE(plan);
    ASSERT_EQ(plan->SlotOrder.size(), uploads.size());
    EXPECT_EQ(plan->SlotOrder[0], &uploads[2]);
    EXPECT_EQ(plan->SlotOrder[1], &uploads[0]);
    EXPECT_EQ(plan->SlotOrder[2], &uploads[4]);
    EXPECT_EQ(plan->SlotOrder[3], &uploads[3]);
    EXPECT_EQ(plan->SlotOrder[4], &uploads[1]);
    EXPECT_EQ(plan->Bytes, 2 * slotBytes + 2 * kFirstPage.size() + kSecondPage.size());

    ASSERT_EQ(plan->Runs.size(), 3u);
    EXPECT_EQ(plan->Runs[0].StagingOffset, 0u);
    EXPECT_EQ(plan->Runs[0].SlotOffset, 0u);
    EXPECT_EQ(plan->Runs[0].Bytes, kFirstPage.size());
    EXPECT_EQ(plan->Runs[1].StagingOffset, kFirstPage.size());
    EXPECT_EQ(plan->Runs[1].SlotOffset, 2 * slotBytes);
    EXPECT_EQ(plan->Runs[1].Bytes, 2 * slotBytes + kFirstPage.size());
    EXPECT_EQ(plan->Runs[2].StagingOffset, 2 * slotBytes + 2 * kFirstPage.size());
    EXPECT_EQ(plan->Runs[2].SlotOffset, 5 * slotBytes);
    EXPECT_EQ(plan->Runs[2].Bytes, kSecondPage.size());

    uploads.back().Ticket.Slot = 2u;
    EXPECT_FALSE(BuildForestPlacementUploadPlan(uploads, 6));
    uploads.back().Ticket.Slot = 6u;
    EXPECT_FALSE(BuildForestPlacementUploadPlan(uploads, 6));
}

TEST_F(PackedPlacementStoreTest, FailedGraphUploadReleasesTheReservationAndPreservesCover)
{
    using namespace Engine::Renderer;
    const auto file = m_Scratch.Path() / "valid.geforest";
    WriteStore(file);
    constexpr std::array<uint32, 3> offsets{0, 6, 12};
    std::string error;
    std::shared_ptr<ForestPlacementRuntime> planner = ForestPlacementRuntime::Open(file, offsets, 1, 1, error);
    ASSERT_TRUE(planner) << error;
    ForestPlacementFrame plan;
    const ForestPlacementView view{{512'000'000.0, 8.0, 512'000'001.0}, {}, 100.0f};
    ASSERT_TRUE(planner->Prepare(view, 1, 0, plan));
    ASSERT_EQ(plan.Reads.size(), 1u);
    const auto ticket = plan.Reads.front();
    EXPECT_FALSE(planner->CanRecordUpload(ticket));
    ASSERT_TRUE(planner->CompleteRead(ticket, kFirstPage));
    EXPECT_TRUE(planner->CanRecordUpload(ticket));
    ForestPlacementUpload upload{ticket, kFirstPage};
    Rendering::RenderGraph::RGFrame graph(nullptr, nullptr, nullptr, nullptr);
    ForestPlacementGpuUploads uploader(planner, {}, 1);
    EXPECT_FALSE(uploader.Record(graph, std::span(&upload, 1)));
    EXPECT_FALSE(planner->CanRecordUpload(ticket));
    EXPECT_FALSE(planner->CompleteUpload(ticket, true));
    ASSERT_TRUE(planner->Prepare(view, 2, 1, plan));
    ASSERT_EQ(plan.StandDraws.size(), 1u);
    EXPECT_EQ(plan.StandDraws.front().IndexCount, 12u);
    EXPECT_TRUE(plan.DetailedDraws.empty());
    ASSERT_EQ(plan.Reads.size(), 1u);
    EXPECT_EQ(plan.Reads.front().Page, 1u);
    EXPECT_EQ(plan.Reads.front().Slot, ticket.Slot);
    EXPECT_NE(plan.Reads.front().Generation, ticket.Generation);
}

TEST_F(PackedPlacementStoreTest, StationaryDemandStillRetriesAndKeepsOtherViewScratchSeparate)
{
    using namespace Engine::Renderer;
    const auto file = m_Scratch.Path() / "valid.geforest";
    WriteStore(file);
    std::string error;
    auto runtime = ForestPlacementRuntime::OpenQuadStand(file, error, 1, 1);
    ASSERT_TRUE(runtime) << error;

    const ForestPlacementView near{{512'000'000.0, 8.0, 512'000'001.0}, {}, 100.0f};
    auto otherView = near;
    otherView.EyeRelativePlanes[0] = {1, 0, 0, -35};
    ForestPlacementFrame plan;
    ASSERT_TRUE(runtime->Prepare(near, 1, 0, plan));
    ASSERT_EQ(plan.Reads.size(), 1u);
    EXPECT_EQ(plan.Reads.front().Page, 0u);
    EXPECT_FALSE(runtime->CompleteRead(plan.Reads.front(), {}));

    std::vector<DrawSubCommand> stand;
    runtime->GatherStand(otherView, stand);
    ASSERT_EQ(stand.size(), 1u);
    EXPECT_EQ(stand.front().FirstIndex, 6u);
    ASSERT_TRUE(runtime->Prepare(near, 2, 1, plan));
    ASSERT_EQ(plan.Reads.size(), 1u);
    EXPECT_EQ(plan.Reads.front().Page, 1u);
    const auto page = plan.Reads.front();
    ASSERT_TRUE(runtime->CompleteRead(page, kSecondPage));
    ASSERT_TRUE(runtime->CompleteUpload(page, true));

    for (uint64 frame = 3; frame < 31; ++frame)
    {
        runtime->GatherStand(otherView, stand);
        ASSERT_TRUE(runtime->Prepare(near, frame, frame - 1, plan));
        ASSERT_EQ(plan.DetailedDraws.size(), 1u);
        EXPECT_EQ(plan.DetailedDraws.front().Ticket, page);
        EXPECT_EQ(plan.VisibleCells, 2u);
        EXPECT_TRUE(plan.Reads.empty());
    }
    ASSERT_TRUE(runtime->Prepare(near, 31, 30, plan));
    ASSERT_EQ(plan.Reads.size(), 1u);
    EXPECT_EQ(plan.Reads.front().Page, 0u);
    EXPECT_EQ(plan.VisibleCells, 2u);
}

TEST_F(PackedPlacementStoreTest, SuppressesOnlyCardsOwnedByTheMatchingMeshSubmission)
{
    using namespace Engine::Renderer;
    using namespace Mathematics;

    const auto file = m_Scratch.Path() / "mesh-ownership.geforest";
    std::array<PackedPlacement, PackedPlacementStore::kPlacementsPerPage> all{};
    auto page = m_Pages[0];
    page.Count = static_cast<uint32>(all.size());
    ASSERT_TRUE(WritePackedPlacementStore(file, std::span(&page, 1), all).empty());
    std::string error;
    auto planner = ForestPlacementRuntime::OpenQuadStand(file, error, 1, 1);
    ASSERT_TRUE(planner) << error;
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
    ASSERT_TRUE(BuildForestPlacementGpuInputs(*planner, plan, camera, 1, 15.0f, {0, 7, 0, 8}, gpu, pages, claims));
    ASSERT_EQ(pages.size(), 1u);
    constexpr std::array<uint32, 8> mask{0x80000001u, 1u, 0u, 0x80000000u, 0x8001u, 0u, 0u, 0u};
    EXPECT_EQ(pages.front().MeshOwned, mask);

    ASSERT_TRUE(BuildForestPlacementGpuInputs(*planner, plan, camera, 1, 15.0f, {0, 7, 0, 8}, gpu, pages));
    ASSERT_EQ(pages.size(), 1u);
    EXPECT_EQ(pages.front().MeshOwned, (std::array<uint32, 8>{}));
}

TEST_F(PackedPlacementStoreTest, BuildsPackedGpuPagesInTheCamerasRenderOrigin)
{
    using namespace Engine::Renderer;
    using namespace Mathematics;
    const auto file = m_Scratch.Path() / "valid.geforest";
    WriteStore(file);
    constexpr std::array<uint32, 3> offsets{0, 6, 12};
    std::string error;
    auto planner = ForestPlacementRuntime::Open(file, offsets, 1, 1, error);
    ASSERT_TRUE(planner) << error;
    const WorldPosition position{512'000'054.0, 8.0, 511'999'990.0};
    const ForestPlacementView visible{position, {}, 100.0f};
    ForestPlacementFrame plan;
    ASSERT_TRUE(planner->Prepare(visible, 1, 0, plan));
    ASSERT_EQ(plan.Reads.size(), 1u);
    const auto ticket = plan.Reads.front();
    ASSERT_EQ(ticket.Page, 1u);
    ASSERT_TRUE(planner->CompleteRead(ticket, kSecondPage));
    ASSERT_TRUE(planner->CompleteUpload(ticket, true));
    ASSERT_TRUE(planner->Prepare(visible, 2, 1, plan));

    Rendering::CameraData camera{};
    camera.view = MakeLookAtLH({}, {0, 0, 1}, {0, 1, 0});
    camera.proj = MakeOrthographicLH_ZO_ReverseZ(-30, 30, -25, 30, 0.1f, 80.0f);
    camera.viewProj = camera.proj * camera.view;
    SetCameraEye(camera, position);
    ForestPlacementGpuView gpu{};
    std::vector<ForestPlacementGpuPage> pages;
    ASSERT_TRUE(BuildForestPlacementGpuInputs(*planner, plan, camera, 1, 15.0f, {0, 7, 0, 8}, gpu, pages));
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
    ASSERT_TRUE(BuildForestPlacementGpuInputs(*planner, missing, camera, 1, 15.0f, {0, 7, 0, 8}, gpu, pages));
    EXPECT_EQ(pages[0].Range[2], 0u);

    camera.proj = MakeOrthographicLH_ZO_ReverseZ(-1, 1.5f, -25, 30, 0.1f, 80.0f);
    camera.viewProj = camera.proj * camera.view;
    SetCameraEye(camera, position);
    ASSERT_TRUE(BuildForestPlacementGpuInputs(*planner, plan, camera, 1, 15.0f, {0, 7, 0, 8}, gpu, pages));
    ASSERT_EQ(pages.size(), 1u);
    EXPECT_EQ(pages[0].Range[2], 0u); // A verified root lies outside the right edge.

    ++plan.DetailedDraws.front().FirstPlacement;
    EXPECT_FALSE(BuildForestPlacementGpuInputs(*planner, plan, camera, 1, 15.0f, {0, 7, 0, 8}, gpu, pages));
    EXPECT_TRUE(pages.empty());
}

} // namespace
} // namespace GameEngine::PageStreaming
