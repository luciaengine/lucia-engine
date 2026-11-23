// The packed placement cull kernel on a headless device: ForestPlacementCull::Declare runs
// forest_placement_cull.comp, and the test reads back the indirect instance count and the visible
// rows. Device-gated: each test skips without a Vulkan device.
#include "ForestPlacementCull.h"
#include "ForestPlacementGpuData.h"
#include "Engine/Rendering/ViewReadbackUtils.h"
#include "PageStreaming/Placement/PackedPlacementStore.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Tests/ScopedEnvVar.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <set>
#include <vector>

namespace GameEngine::Engine::Renderer
{
namespace
{
using PageStreaming::PackedPlacement;
using PageStreaming::PackedPlacementStore;
using namespace Rendering;
using namespace Rendering::RenderGraph;

struct Pools
{
    RGResourcePool Persistent;
    RGTransientPool Transient;
    RGUploadRing Ring;
    explicit Pools(IDevice* device) : Persistent(device), Transient(device), Ring(device, 2, 256 * 1024) {}
};

// The pipeline compiles in the background: Declare returns nothing until it is ready.
constexpr int kMaxDeclareAttempts = 200;
// More rows than any test can make visible.
constexpr uint32 kReadRows = 300;

struct CullResult
{
    uint32 Instances = 0;
    std::vector<std::pair<uint32, uint32>> Rows; // placement slot, page index
};

class ForestPlacementCullTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        DeviceDesc desc{};
        desc.preferredAPI = GraphicsAPI::Vulkan;
        desc.enableDynamicRendering = true;
        m_Device = DeviceFactory::CreateDevice(desc);
        if (!m_Device || !m_Device->Initialize(desc))
            GTEST_SKIP() << "No Vulkan device";
        constexpr uint32 kPerPage = PackedPlacementStore::kPlacementsPerPage;
        m_Slots = m_Device->CreateBuffer({.size = 2 * kPerPage * sizeof(PackedPlacement),
            .usage = static_cast<uint32>(BufferUsage::Storage | BufferUsage::TransferDst),
            .memoryUsage = BufferMemoryUsage::DeviceLocal, .debugName = "ForestCullTest.Slots"});
        ASSERT_TRUE(m_Slots.IsValid());
        std::vector<PackedPlacement> placements(2 * kPerPage);
        for (uint32 i = 0; i < 6; ++i)
            placements[i] = {static_cast<uint16>(i * 100), 0, 0, 0, 0}; // x = i meters, scale 0.75
        m_Device->UpdateBuffer(m_Slots, 0, placements.size() * sizeof(PackedPlacement), placements.data());
        ASSERT_TRUE(m_Cull.Initialize(*m_Device));
    }

    void TearDown() override
    {
        if (!m_Device)
            return;
        m_Device->WaitForIdle();
        m_Device->DestroyBuffer(m_Slots);
        m_Device->Shutdown();
    }

    CullResult Run(std::vector<ForestPlacementGpuPage> pages, uint32 frameIndex)
    {
        ForestPlacementGpuView view{};
        // Keep x <= 2.5 m; the other planes accept everything.
        view.Planes = {{{-1, 0, 0, 2.5f}, {0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}}};
        view.EyeRelativeAndRadius = {0, 0, 0, 0.1f};
        Pools pools(m_Device.get());
        RGFrame graph(m_Device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        EXPECT_TRUE(m_Device->BeginFrame());
        graph.BeginFrame(frameIndex);
        std::optional<ForestPlacementCull::DrawBuffers> buffers;
        for (int attempt = 0; attempt < kMaxDeclareAttempts && !buffers; ++attempt)
            buffers = m_Cull.Declare(graph, "ForestCullTest", m_Slots, 2, view, pages);
        EXPECT_TRUE(buffers);
        if (!buffers)
            return {};
        auto args = RequestBufferReadbackRG(m_Device.get(), graph, buffers->Args, 0, sizeof(ForestPlacementIndirectDraw), "ForestCullTest.Args");
        auto rows = RequestBufferReadbackRG(m_Device.get(), graph, buffers->Visible, 0, kReadRows * sizeof(ForestPlacementGpuVisible), "ForestCullTest.Rows");
        graph.Execute();
        OnFrameSubmittedReadbacksRG(graph, graph.SubmissionToken());
        m_Device->WaitForIdle();
        CullResult result;
        BufferReadbackResult bytes{};
        EXPECT_TRUE(args && args->TryGet(bytes));
        ForestPlacementIndirectDraw draw{};
        EXPECT_EQ(bytes.bytes.size(), sizeof(draw));
        std::memcpy(&draw, bytes.bytes.data(), sizeof(draw));
        EXPECT_EQ(draw.IndexCount, 6u);
        result.Instances = draw.InstanceCount;
        BufferReadbackResult rowBytes{};
        EXPECT_TRUE(rows && rows->TryGet(rowBytes));
        for (uint32 i = 0; i < std::min(draw.InstanceCount, kReadRows); ++i)
        {
            ForestPlacementGpuVisible row{};
            std::memcpy(&row, rowBytes.bytes.data() + i * sizeof(row), sizeof(row));
            result.Rows.emplace_back(row.Placement, row.Page);
        }
        std::ranges::sort(result.Rows);
        return result;
    }

    static ForestPlacementGpuPage Page(uint32 firstSlot, uint32 count, bool inside)
    {
        ForestPlacementGpuPage page{};
        page.Range = {firstSlot, count, inside ? 1u : 0u, 0};
        return page;
    }

    Rendering::Tests::ScopedEnvVar m_Headless{"GE_HEADLESS_TEST", "1"};
    std::unique_ptr<IDevice> m_Device;
    BufferHandle m_Slots;
    ForestPlacementCull m_Cull;
};

using Rows = std::vector<std::pair<uint32, uint32>>;

TEST_F(ForestPlacementCullTest, PartialPageTestsEachRoot)
{
    const auto result = Run({Page(0, 6, false)}, 0);
    EXPECT_EQ(result.Instances, 3u);
    EXPECT_EQ(result.Rows, (Rows{{0, 0}, {1, 0}, {2, 0}}));
}

TEST_F(ForestPlacementCullTest, MeshOwnedBitDropsOneCard)
{
    auto page = Page(0, 6, false);
    page.MeshOwned[0] = 1u << 1;
    const auto result = Run({page}, 0);
    EXPECT_EQ(result.Rows, (Rows{{0, 0}, {2, 0}}));
}

TEST_F(ForestPlacementCullTest, InsidePageSkipsTheFrustumAndKeepsOwnership)
{
    auto result = Run({Page(0, 6, true)}, 0);
    EXPECT_EQ(result.Instances, 6u);
    auto page = Page(0, 6, true);
    page.MeshOwned[0] = 1u << 5;
    result = Run({page}, 1);
    EXPECT_EQ(result.Rows, (Rows{{0, 0}, {1, 0}, {2, 0}, {3, 0}, {4, 0}}));
}

TEST_F(ForestPlacementCullTest, TwoPagesCompactIntoDisjointTailRanges)
{
    constexpr uint32 kPerPage = PackedPlacementStore::kPlacementsPerPage;
    auto second = Page(kPerPage, kPerPage, true);
    second.MeshOwned[4] = 1u << 15; // local placement 143
    const auto result = Run({Page(0, 6, false), second}, 0);
    EXPECT_EQ(result.Instances, 3u + kPerPage - 1u);
    std::set<uint32> slots;
    for (const auto& [slot, pageIndex] : result.Rows)
    {
        slots.insert(slot);
        EXPECT_EQ(pageIndex, slot < kPerPage ? 0u : 1u);
    }
    EXPECT_EQ(slots.size(), result.Rows.size());
    EXPECT_FALSE(slots.contains(kPerPage + 143));
    EXPECT_TRUE(slots.contains(kPerPage + 142));
}

} // namespace
} // namespace GameEngine::Engine::Renderer
