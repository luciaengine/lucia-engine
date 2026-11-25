// The packed placement cull kernel on a headless device: ForestPlacementCull::Declare runs
// forest_placement_cull.comp, and the test reads back the indirect instance count and the visible
// rows on the native GPU backend.
#include "ForestPlacementCull.h"
#include "ForestPlacementGpuData.h"
#include "Engine/Rendering/ShadowMapRenderFeature.h"
#include "Engine/Rendering/ViewReadbackUtils.h"
#include "Mathematics/MatrixOps.h"
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
#if defined(__APPLE__)
        desc.preferredAPI = GraphicsAPI::Metal;
#else
        desc.preferredAPI = GraphicsAPI::Vulkan;
#endif
        desc.enableDynamicRendering = true;
        m_Device = DeviceFactory::CreateDevice(desc);
        if (!m_Device || !m_Device->Initialize(desc))
            GTEST_SKIP() << "No native graphics device";
        constexpr uint32 kPerPage = PackedPlacementStore::kPlacementsPerPage;
        m_Slots = m_Device->CreateBuffer({.size = 2 * kPerPage * sizeof(PackedPlacement),
            .usage = static_cast<uint32>(BufferUsage::Storage | BufferUsage::TransferDst),
            .memoryUsage = BufferMemoryUsage::DeviceLocal, .debugName = "ForestCullTest.Slots"});
        ASSERT_TRUE(m_Slots.IsValid());
        std::vector<PackedPlacement> placements(2 * kPerPage);
        for (uint32 i = 0; i < 6; ++i)
            placements[i] = {static_cast<uint16>(i * 100), 0, 0, 0, 0}; // x = i meters, scale 0.75
        m_Device->UpdateBuffer(m_Slots, 0, placements.size() * sizeof(PackedPlacement), placements.data());
        ASSERT_TRUE(m_Cull.Initialize(*m_Device, false));
    }

    void TearDown() override
    {
        if (!m_Device || !m_Slots.IsValid())
            return;
        m_Device->WaitForIdle();
        m_Device->DestroyBuffer(m_Slots);
        m_Device->Shutdown();
    }

    // A view with a bound radius of 0.1 m whose first planes are `first` and `second`; the other
    // planes accept everything.
    static ForestPlacementGpuView MakeView(const Mathematics::Vector4& first, const Mathematics::Vector4& second)
    {
        ForestPlacementGpuView view{};
        view.Planes = {{first, second, {0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}}};
        view.EyeRelativeAndRadius = {0, 0, 0, 0.1f};
        return view;
    }

    // Keeps x <= 2.5 m.
    CullResult Run(std::vector<ForestPlacementGpuPage> pages, uint32 frameIndex)
    {
        return Run(std::move(pages), frameIndex, MakeView({-1, 0, 0, 2.5f}, {0, 0, 0, 1}));
    }

    CullResult Run(std::vector<ForestPlacementGpuPage> pages, uint32 frameIndex, const ForestPlacementGpuView& view,
                   TextureHandle depth = {}, SamplerHandle sampler = {})
    {
        Pools pools(m_Device.get());
        RGFrame graph(m_Device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        EXPECT_TRUE(m_Device->BeginFrame());
        graph.BeginFrame(frameIndex);
        const auto hzb = depth.IsValid() ? graph.ImportExternalTexture(
            "ForestCullTest.HZB", depth, ResourceState::ShaderResource, TextureFormat::R32_FLOAT) : RGTexture{};
        std::optional<ForestPlacementCull::DrawBuffers> buffers;
        for (int attempt = 0; attempt < kMaxDeclareAttempts && !buffers; ++attempt)
            buffers = m_Cull.Declare(graph, "ForestCullTest", m_Slots, 2, view, pages, 6, nullptr, hzb, sampler);
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

TEST_F(ForestPlacementCullTest, DepthPyramidRejectsOnlyACompletelyHiddenPage)
{
    constexpr float kOccluderDepth = 0.7f;
    const auto texture = m_Device->CreateTexture({
        .format = static_cast<uint32>(TextureFormat::R32_FLOAT),
        .usage = static_cast<uint32>(TextureUsage::ShaderResource | TextureUsage::TransferDst)});
    ASSERT_TRUE(texture.IsValid());
    const auto sampler = m_Device->CreateSampler(SamplerDesc::PointClamp("ForestCullTest.HZB"));
    ASSERT_TRUE(sampler.IsValid());
    const auto staging = m_Device->CreateUploadBuffer(sizeof(kOccluderDepth), "ForestCullTest.HZB.Upload");
    ASSERT_TRUE(staging.IsValid());
    m_Device->UpdateBuffer(staging, 0, sizeof(kOccluderDepth), &kOccluderDepth);
    auto commands = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
    commands->Begin();
    commands->CopyBufferToTextureSubresource(staging, texture, 0, 0, 1, 1, 0, sizeof(kOccluderDepth),
                                              1, 0, 0, 0, ResourceState::Undefined);
    commands->Barrier(ResourceBarrier::CreateTextureBarrier(texture, ResourceState::CopyDest,
                                                              ResourceState::ShaderResource));
    commands->End();
    m_Device->ExecuteCommandLists({commands.get()});
    m_Device->WaitForIdle();
    m_Device->DestroyBuffer(staging);

    constexpr uint32 kPerPage = PackedPlacementStore::kPlacementsPerPage;
    auto hidden = Page(0u, 3u, true);
    hidden.BoundsMinRelative = {-0.1f, -0.1f, 0.2f, 0.0f};
    hidden.BoundsMaxRelative = {0.1f, 0.1f, 0.4f, 0.0f};
    auto visible = Page(kPerPage, 1u, true);
    visible.BoundsMinRelative = {-0.1f, -0.1f, 0.5f, 0.0f};
    visible.BoundsMaxRelative = {0.1f, 0.1f, 0.8f, 0.0f};
    auto view = MakeView({0, 0, 0, 1}, {0, 0, 0, 1});
    view.ViewProjectionRelative = Mathematics::Matrix4x4::Identity();
    EXPECT_EQ(Run({hidden, visible}, 0, view).Instances, 4u);
    CullResult result;
    for (uint32 frame = 1; frame < 8; ++frame)
    {
        result = Run({hidden, visible}, frame, view, texture, sampler);
        if (result.Instances == 1u)
            break;
    }
    EXPECT_EQ(result.Rows, (Rows{{kPerPage, 1u}}));
    m_Device->DestroySampler(sampler);
    m_Device->DestroyTexture(texture);
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

TEST_F(ForestPlacementCullTest, EachFieldOfAPlacementPlacesItsRoot)
{
    // X, Z and height in centimeters, yaw, scale. The planes keep z <= 3 m and y <= 2.5 m.
    const std::vector<PackedPlacement> placements{
        {0, 300, 0, 0, 0},     // z = 3 m: on the plane, kept
        {0, 100, 0, 77, 0},    // z = 1 m, any yaw: kept
        {0, 100, 500, 0, 0},   // 5 m high: culled by the height plane
        {0, 310, 0, 0, 255},   // 0.1 m past the plane, radius 0.125 m at scale 1.25: kept
        {0, 310, 0, 0, 0},     // 0.1 m past the plane, radius 0.075 m at scale 0.75: culled
        {400, 310, 0, 0, 128}, // x does not matter to these planes, radius 0.1 m at scale 1.0: kept
    };
    m_Device->UpdateBuffer(m_Slots, 0, placements.size() * sizeof(PackedPlacement), placements.data());
    const auto result = Run({Page(0, 6, false)}, 0, MakeView({0, 0, -1, 3.0f}, {0, -1, 0, 2.5f}));
    EXPECT_EQ(result.Rows, (Rows{{0, 0}, {1, 0}, {3, 0}, {5, 0}}));
}

TEST_F(ForestPlacementCullTest, ShadowCullUsesTheCascadeCasterPlanes)
{
    using namespace Mathematics;
    // The light at x = 2.5 m looks along +X and covers 1 m on each side.
    const std::vector<PackedPlacement> placements{
        {0, 0, 0, 0, 0},     // x = 0: nearer the light than its near plane, kept
        {300, 0, 0, 0, 0},   // x = 3 m: kept
        {300, 0, 500, 0, 0}, // 5 m high: outside the caster footprint, culled
    };
    m_Device->UpdateBuffer(m_Slots, 0, placements.size() * sizeof(PackedPlacement), placements.data());
    const Matrix4x4 lightView = MakeLookAtLH({2.5f, 0, 0}, {3.5f, 0, 0}, {0, 1, 0});
    DepthEmitCascades cascades{};
    cascades.Count = 1;
    cascades.Cascades[0].LightViewProjectionRel = MakeOrthographicLH_ZO_ReverseZ(-1, 1, -1, 1, 0.1f, 20.0f) * lightView;
    ShadowMapRenderFeature::BuildCascadeCasterCullPlanes(cascades.Cascades[0].LightViewProjectionRel,
                                                         cascades.Cascades[0].CasterPlanes.data());

    const auto source = MakeForestShadowGpuViewSource(cascades, 0);
    ForestPlacementGpuView view{};
    view.ViewProjectionRelative = source.ViewProjectionRelative;
    view.Planes = source.Planes;
    view.EyeRelativeAndRadius = {0, 0, 0, 0.1f};
    const auto result = Run({Page(0, 3, false)}, 0, view);
    EXPECT_EQ(result.Rows, (Rows{{0, 0}, {1, 0}}));
}

} // namespace
} // namespace GameEngine::Engine::Renderer
