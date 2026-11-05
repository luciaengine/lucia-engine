#include <gtest/gtest.h>

#include "Ocean/OceanSplineRaster.h"
#include "Ocean/OceanTypes.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "TestTempDir.h"
#include "Types/Fnv1a.h"

#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;
using namespace GameEngine::Ocean;

namespace
{
constexpr uint32 kResolution = 64u;
constexpr float kTexelMeters = 1.0f;
// The lower edge sits 0.05 m above a texel center, where a field that steps at
// the edge puts its reconstructed boundary about half a texel away from it.
constexpr float kRibbonCenterZ = 31.75f;
constexpr float kRibbonHalfWidth = 3.3f;
// Storage precision over a two-texel ramp resolves the boundary to about 0.01 m.
constexpr float kContourTolerance = 0.05f;
constexpr uint32 kDepthField = 0u;
constexpr uint32 kClipField = 2u;
constexpr float kDeepWaterMeters = 60000.0f;
constexpr float kSaturationDepthMeters = 10.0f;

float HalfToFloat(uint16 half)
{
    const uint32 sign = (half >> 15) & 1u, exponent = (half >> 10) & 0x1fu, mantissa = half & 0x3ffu;
    const float magnitude = exponent == 0u ? std::ldexp(static_cast<float>(mantissa), -24)
                                           : std::ldexp(static_cast<float>(mantissa | 0x400u),
                                                        static_cast<int>(exponent) - 25);
    return sign ? -magnitude : magnitude;
}

// Draws one straight ribbon along X into a single-layer cascade of `format`
// cleared to `clearValue`, one meter per texel, and reads the layer back.
std::vector<float> RasterizeRibbon(IDevice& device, uint32 field, TextureFormat format, float clearValue,
                                   const OceanRibbonStyle& style)
{
    const bool halfFloat = format == TextureFormat::R16_FLOAT || format == TextureFormat::R16G16_FLOAT;
    const size_t channels = format == TextureFormat::R16G16_FLOAT ? 2u :
                            format == TextureFormat::RGBA8_UNORM ? 4u : 1u;
    const size_t texelBytes = channels * (halfFloat ? 2u : 1u);
    TextureDesc desc{};
    desc.width = kResolution;
    desc.height = kResolution;
    desc.arrayLayers = 1;
    desc.format = static_cast<uint32>(format);
    desc.usage = static_cast<uint32>(TextureUsage::UnorderedAccess | TextureUsage::ShaderResource |
                                     TextureUsage::TransferSrc | TextureUsage::TransferDst);
    desc.flags = TextureCreateFlags::ForceArrayView;
    desc.persistent = true;
    desc.debugName = "OceanSplineRasterTest.Field";
    const TextureHandle texture = device.CreateTexture(desc);
    EXPECT_TRUE(texture.IsValid());
    if (!texture.IsValid())
        return {};
    const float clear[4] = {clearValue, 0, 0, 0};
    auto commands = device.CreateCommandList(IDevice::QueueType::Graphics);
    commands->Begin();
    commands->ClearColorImageSubresource(texture, 0, 0, clear);
    commands->Barrier(ResourceBarrier::CreateTextureBarrier(texture, ResourceState::CopyDest,
                                                            ResourceState::ShaderResource));
    commands->End();
    device.ExecuteCommandLists({commands.get()});
    device.WaitForIdle();

    OceanSplineRaster raster;
    raster.SetTriangles(BuildOceanRibbon({{2, 0, kRibbonCenterZ, 1, 0}, {62, 0, kRibbonCenterZ, 1, 0}}, style));
    OceanCascadeLayoutGPU layout{};
    layout.CascadeOriginScale[0][2] = kTexelMeters;
    layout.CascadeOriginScale[0][3] = 1.0f;
    layout.LodCount = 1;
    RenderGraph::RGResourcePool persistent(&device);
    RenderGraph::RGTransientPool transient(&device);
    RenderGraph::RGUploadRing ring(&device, 2, 262144);
    {
        RenderGraph::RGFrame frame(&device, &persistent, &transient, &ring);
        EXPECT_TRUE(device.BeginFrame());
        frame.BeginFrame(0);
        const auto target = frame.ImportExternalTexture("OceanSplineRasterTest.Field", texture,
                                                        ResourceState::ShaderResource, format, 1, 1);
        const auto writerId = static_cast<RenderGraph::RGPassId>(frame.Graph().PassCount());
        EXPECT_TRUE(raster.Declare(frame, &device, field, target, layout, kResolution));
        const RenderGraph::RGPass writer{writerId};
        bool observedBefore = false, observedAfter = false;
        uint32 beforeCount = 0;
        const auto before = frame.AddPass("OceanSpline.BeforeRecord", PassPhase::kEarlySetup,
            [](RenderGraph::RGPassBuilder& pass) { pass.PreventCulling(); },
            [&](RenderGraph::RGContext&)
            {
                observedBefore = true;
                if (device.GetAPI() == GraphicsAPI::Vulkan)
                {
                    beforeCount = device.GetDescriptorAllocatorStats().TransientAllocated;
                    EXPECT_GT(beforeCount, 0u);
                }
            });
        const auto after = frame.AddPass("OceanSpline.AfterRecord", PassPhase::kEarlySetup,
            [](RenderGraph::RGPassBuilder& pass) { pass.PreventCulling(); },
            [&](RenderGraph::RGContext&)
            {
                observedAfter = true;
                if (device.GetAPI() == GraphicsAPI::Vulkan)
                {
                    const auto afterCount = device.GetDescriptorAllocatorStats().TransientAllocated;
                    ::testing::Test::RecordProperty("DescriptorsBeforeRaster", beforeCount);
                    ::testing::Test::RecordProperty("DescriptorsAfterRaster", afterCount);
                    EXPECT_EQ(afterCount, beforeCount);
                }
            });
        frame.AddOrderingEdge(before, writer);
        frame.AddOrderingEdge(writer, after);
        bool allowSubmission = false;
        const auto abort = frame.AddPass("OceanSpline.Abort", PassPhase::kFinalize,
            [&](RenderGraph::RGPassBuilder& pass)
            {
                pass.PreventCulling();
                pass.Prepare([&](RenderGraph::RGPrepareContext&)
                {
                    if (device.GetAPI() == GraphicsAPI::Vulkan)
                        EXPECT_GT(device.GetDescriptorAllocatorStats().TransientAllocated, 0u);
                    return allowSubmission;
                });
            }, [](RenderGraph::RGContext&) {});
        frame.AddOrderingEdge(after, abort);
        // The pass owns its original triangles and tile list even when the CPU set changes.
        raster.SetTriangles({});
        const auto submitted = frame.SubmittedFrameCount();
        for (int retry = 0; retry < 2; ++retry)
        {
            frame.Execute();
            EXPECT_EQ(frame.SubmittedFrameCount(), submitted);
            EXPECT_FALSE(observedBefore);
            EXPECT_FALSE(observedAfter);
        }
        allowSubmission = true;
        frame.Execute();
        EXPECT_EQ(frame.SubmittedFrameCount(), submitted + 1u);
        EXPECT_TRUE(observedBefore);
        EXPECT_TRUE(observedAfter);
        device.FinalizeFrame();
        device.WaitForIdle();
    }

    const BufferHandle readback = device.CreateReadbackBuffer(kResolution * kResolution * texelBytes);
    commands = device.CreateCommandList(IDevice::QueueType::Graphics);
    commands->Begin();
    commands->Barrier(ResourceBarrier::CreateTextureBarrier(texture, ResourceState::ShaderResource,
                                                            ResourceState::CopySource));
    commands->CopyTextureToBuffer(texture, readback, kResolution, kResolution);
    commands->End();
    device.ExecuteCommandLists({commands.get()});
    device.WaitForIdle();
    std::vector<float> values(kResolution * kResolution * channels);
    if (const auto* mapped = static_cast<const uint8*>(device.MapBuffer(readback)))
    {
        ::testing::Test::RecordProperty("Pixels", std::to_string(Hashing::Fnv1a64(mapped, kResolution * kResolution * texelBytes)));
        for (size_t i = 0; i < values.size(); ++i)
        {
            if (halfFloat)
            {
                uint16 half = 0;
                std::memcpy(&half, mapped + i * 2u, 2u);
                values[i] = HalfToFloat(half);
            }
            else
                values[i] = mapped[i] / 255.0f;
        }
        device.UnmapBuffer(readback);
    }
    device.DestroyBuffer(readback);
    device.DestroyTexture(texture);
    return values;
}

// Where the bilinear reconstruction of one texel column first crosses
// `threshold`, walking from texel row `fromZ` toward `toZ`, in meters.
float Crossing(const std::vector<float>& values, uint32 column, int fromZ, int toZ, float threshold)
{
    const int step = toZ > fromZ ? 1 : -1;
    for (int z = fromZ; z != toZ; z += step)
    {
        const float a = values[static_cast<size_t>(z) * kResolution + column];
        const float b = values[static_cast<size_t>(z + step) * kResolution + column];
        if ((a < threshold) != (b < threshold))
            return (static_cast<float>(z) + 0.5f + static_cast<float>(step) * (threshold - a) / (b - a)) *
                   kTexelMeters;
    }
    return -1.0f;
}

void ExpectBoundariesOnTheTrueEdges(const std::vector<float>& values, float threshold)
{
    ASSERT_EQ(values.size(), static_cast<size_t>(kResolution * kResolution));
    const int center = static_cast<int>(kRibbonCenterZ / kTexelMeters);
    for (uint32 column = 16; column < 48; column += 8)
    {
        EXPECT_NEAR(Crossing(values, column, 0, center, threshold), kRibbonCenterZ - kRibbonHalfWidth,
                    kContourTolerance)
            << "lower edge, column " << column;
        EXPECT_NEAR(Crossing(values, column, static_cast<int>(kResolution) - 1, center, threshold),
                    kRibbonCenterZ + kRibbonHalfWidth, kContourTolerance)
            << "upper edge, column " << column;
    }
}
class OceanSplineRasterTest : public ::testing::Test
{
  protected:
    virtual GraphicsAPI RequestedAPI() const { return GraphicsAPI::Vulkan; }
    void SetUp() override
    {
        DeviceDesc desc{};
        desc.preferredAPI = RequestedAPI();
        desc.enableSwapchain = false;
        desc.enableDebugLayer = false;
        desc.pipelineCacheDirectory = m_Cache.Path().string();
        // Vulkan's pool counter is the readiness witness; Metal's counter is unsupported.
        desc.descriptorBuffers = DescriptorBufferMode::Disabled;
        m_Device = DeviceFactory::CreateDevice(desc);
        if (!m_Device || !m_Device->Initialize(desc))
            GTEST_SKIP() << "Requested ocean spline raster backend unavailable";
        ASSERT_EQ(m_Device->GetAPI(), RequestedAPI());
    }
    void TearDown() override
    {
        if (m_Device)
        {
            m_Device->WaitForIdle();
            m_Device->Shutdown();
        }
    }
    void CheckClipEdge()
    {
        OceanRibbonStyle style{};
        style.Width = kRibbonHalfWidth;
        style.Feather = 0.1f * kRibbonHalfWidth;
        style.Flags = 4u;
        ExpectBoundariesOnTheTrueEdges(
            RasterizeRibbon(*m_Device, kClipField, TextureFormat::R8_UNORM, 0.0f, style), 0.5f);
    }
    void CheckDepthEdge()
    {
        OceanRibbonStyle style{};
        style.Width = kRibbonHalfWidth;
        style.Depth = 2.0f;
        style.DepthSaturation = kSaturationDepthMeters;
        style.Flags = 1u;
        ExpectBoundariesOnTheTrueEdges(
            RasterizeRibbon(*m_Device, kDepthField, TextureFormat::R16_FLOAT, kDeepWaterMeters, style),
            kSaturationDepthMeters);
    }
    void CheckFlow()
    {
        OceanRibbonStyle style{};
        style.Width = kRibbonHalfWidth;
        style.FlowSpeed = 2.5f;
        style.Flags = 2u;
        const auto values = RasterizeRibbon(*m_Device, 1u, TextureFormat::R16G16_FLOAT, 0.0f, style);
        ASSERT_EQ(values.size(), size_t(kResolution) * kResolution * 2u);
        const size_t inside = (31u * kResolution + 32u) * 2u;
        EXPECT_FLOAT_EQ(values[inside], 2.5f);
        EXPECT_FLOAT_EQ(values[inside + 1u], 0.0f);
        EXPECT_FLOAT_EQ(values[(3u * kResolution + 32u) * 2u], 0.0f);
    }
    void CheckAlbedo()
    {
        OceanRibbonStyle style{};
        style.Width = kRibbonHalfWidth;
        style.Flags = 8u;
        style.Color[0] = 0.2f;
        style.Color[1] = 0.4f;
        style.Color[2] = 0.8f;
        style.Color[3] = 0.6f;
        const auto values = RasterizeRibbon(*m_Device, 3u, TextureFormat::RGBA8_UNORM, 0.0f, style);
        ASSERT_EQ(values.size(), size_t(kResolution) * kResolution * 4u);
        const size_t inside = (31u * kResolution + 32u) * 4u;
        for (size_t channel = 0; channel < 4u; ++channel)
            EXPECT_NEAR(values[inside + channel], style.Color[channel], 1.0f / 255.0f);
        EXPECT_FLOAT_EQ(values[(3u * kResolution + 32u) * 4u + 3u], 0.0f);
    }
    std::unique_ptr<IDevice> m_Device;
    TestUtils::ScopedTempDir m_Cache{TestUtils::MakeUniqueTempDirectory("ocean_spline_preparation")};
};
} // namespace

TEST_F(OceanSplineRasterTest, ClipHalfCoverageFollowsTheTrueEdgeBetweenTexels) { CheckClipEdge(); }
TEST_F(OceanSplineRasterTest, DepthBandSaturationContourFollowsTheTrueEdgeBetweenTexels) { CheckDepthEdge(); }
TEST_F(OceanSplineRasterTest, FlowKeepsTheDeclaredTrianglesAndTileBins) { CheckFlow(); }
TEST_F(OceanSplineRasterTest, AlbedoKeepsTheDeclaredTrianglesAndTileBins) { CheckAlbedo(); }

#if defined(__APPLE__)
namespace
{
class MetalOceanSplineRasterTest : public OceanSplineRasterTest
{
  protected:
    GraphicsAPI RequestedAPI() const override { return GraphicsAPI::Metal; }
};
}
TEST_F(MetalOceanSplineRasterTest, ClipHalfCoverageFollowsTheTrueEdgeBetweenTexels) { CheckClipEdge(); }
TEST_F(MetalOceanSplineRasterTest, DepthBandSaturationContourFollowsTheTrueEdgeBetweenTexels) { CheckDepthEdge(); }
TEST_F(MetalOceanSplineRasterTest, FlowKeepsTheDeclaredTrianglesAndTileBins) { CheckFlow(); }
TEST_F(MetalOceanSplineRasterTest, AlbedoKeepsTheDeclaredTrianglesAndTileBins) { CheckAlbedo(); }
#endif
