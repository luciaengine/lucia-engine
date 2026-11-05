#include <gtest/gtest.h>

#include "Engine/Rendering/Pipeline/Nodes/SkyRenderNode.h"
#include "Engine/Rendering/Pipeline/RenderPipeline.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/SkyRenderFeature.h"
#include "Mathematics/HalfFloat.h"
#include "Mathematics/Matrix4x4.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "TestTempDir.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;
using namespace GameEngine::Engine::Renderer;
using namespace GameEngine::Engine::Renderer::Pipeline;

namespace
{
constexpr uint32_t kExtent = 64;
constexpr uint64_t kReadbackBytes = kExtent * kExtent * 4u * sizeof(uint16_t);

struct SkyFramePools
{
    RenderGraph::RGResourcePool Persistent;
    RenderGraph::RGTransientPool Transient;
    RenderGraph::RGUploadRing Uploads;
    explicit SkyFramePools(IDevice* device) : Persistent(device), Transient(device), Uploads(device, 3, 262144) {}
};

struct SkyDeviceFrame
{
    IDevice& Device;
    bool Active;
    explicit SkyDeviceFrame(IDevice& device) : Device(device), Active(device.BeginFrame()) {}
    ~SkyDeviceFrame() { Finish(); }
    void Finish()
    {
        if (!Active)
            return;
        Device.FinalizeFrame();
        Device.WaitForIdle();
        Active = false;
    }
};

class SkyLutRestingLayoutTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        DeviceDesc desc{};
        desc.preferredAPI = GraphicsAPI::Vulkan;
        desc.enableSwapchain = false;
        desc.enableDebugLayer = true;
        desc.enableDynamicRendering = true;
        desc.descriptorBuffers = DescriptorBufferMode::Disabled;
        desc.pipelineCacheDirectory = m_Cache.Path().string();
        m_Device = DeviceFactory::CreateDevice(desc);
        if (!m_Device)
            GTEST_SKIP() << "Vulkan device unavailable";
        // Vulkan validation telemetry is process-global and survives device destruction.
        // Capture before Initialize so this test still counts initialization errors.
        m_ValidationBaseline = m_Device->GetValidationStats();
        RecordProperty("validation_baseline_errors", std::to_string(m_ValidationBaseline.ErrorCount));
        RecordProperty("validation_baseline_overflow", std::to_string(m_ValidationBaseline.OverflowCount));
        if (!m_Device->Initialize(desc))
            GTEST_SKIP() << "Vulkan device initialization unavailable";
        ASSERT_EQ(m_Device->GetAPI(), GraphicsAPI::Vulkan);
        if (!m_Device->GetValidationStats().Enabled)
            GTEST_SKIP() << "Vulkan validation layer unavailable; resting-layout regression not exercised";
        m_Services = std::make_unique<RenderServices>();
        ASSERT_TRUE(m_Services->Initialize(m_Device.get()));
        m_Sky = &m_Services->EnsureFeature<SkyRenderFeature>();
        ASSERT_TRUE(m_Sky->Initialize(m_Device.get()));
        SkySettings settings{};
        settings.showSunDisk = false;
        settings.showMoonDisk = false;
        settings.fallingStarsEnabled = false;
        settings.nightSkyBlend = 0.0f;
        m_Sky->SetSettings(settings, SkySystemState{});
        ASSERT_FALSE(m_Sky->GetRenderer().IsTransmittanceComputed());
        ASSERT_EQ(m_Sky->GetRenderer().GetSkyViewLutComputedDigest(), 0u);

        const auto cameraId = m_Services->Views().AllocateCamera("SkyResting.Camera");
        CameraData camera{};
        const auto view = Mathematics::Matrix4x4::LookAt(
            Mathematics::Vector3(0, 1, 0), Mathematics::Vector3(0, 1, 1), Mathematics::Vector3(0, 1, 0));
        const auto projection = Mathematics::Matrix4x4::PerspectiveReverseZ(1.0471976f, 1.0f, 0.1f, 100.0f);
        const auto viewProjection = projection * view;
        std::memcpy(camera.view, view.Data(), sizeof(camera.view));
        std::memcpy(camera.proj, projection.Data(), sizeof(camera.proj));
        std::memcpy(camera.viewProj, viewProjection.Data(), sizeof(camera.viewProj));
        camera.cameraPos[1] = 1.0f;
        m_Services->Views().SetCameraData(cameraId, camera);
        m_View = m_Services->Views().AllocateView("SkyResting.View", cameraId);
        m_Services->Views().SetViewRenderLayerMask(m_View, 1);
        m_Services->Views().SetViewTargets(m_View, 0, 0, 0, ViewClearConfig{});
        ASSERT_TRUE(m_Registry.Register("SkyRender", [] { return std::make_unique<Nodes::SkyRenderNode>(); }, true));
        RenderPipelineBlueprint blueprint;
        blueprint.pipelineName = "SkyResting";
        RenderPipelineBlueprint::Pass pass{};
        pass.id = "Sky";
        pass.type = "SkyRender";
        pass.enabled = pass.perView = true;
        pass.passJson = R"({"id":"Sky","type":"SkyRender"})";
        blueprint.passes.push_back(pass);
        m_Pipeline = std::make_unique<RenderPipelineInstance>(*m_Services, m_Registry);
        m_Pipeline->SetBlueprint(blueprint);
        m_Readback = m_Device->CreateReadbackBuffer(kReadbackBytes, "SkyResting.ColorReadback");
        ASSERT_TRUE(m_Readback.IsValid());
        CheckValidation();
    }

    void TearDown() override
    {
        if (!m_Device)
            return;
        m_Device->WaitForIdle();
        m_Pipeline.reset();
        if (m_Services)
            m_Services->Shutdown();
        m_Services.reset();
        if (m_Readback.IsValid())
            m_Device->DestroyBuffer(m_Readback);
        m_Device->Shutdown();
    }

    void CheckComputePasses(const RenderGraph::RGFrame& frame, bool expected, bool executed)
    {
        for (const char* suffix : {"Transmittance", "MultiscatterLUT", "SkyViewLUT"})
        {
            const std::string name = std::string("Pipeline.SkyResting.Sky.") + suffix;
            uint32_t found = 0;
            for (RenderGraph::RGPassId id = 0; id < frame.Graph().PassCount(); ++id)
                if (name == frame.Graph().PassName(id))
                {
                    ++found;
                    if (executed)
                        EXPECT_FALSE(frame.Graph().IsCulled(id)) << name;
                }
            EXPECT_EQ(found, expected ? 1u : 0u) << name;
        }
    }

    void DeclareFrame(RenderGraph::RGFrame& frame, uint64_t index, bool draw)
    {
        frame.BeginFrame(index);
        m_Services->BeginWorldDrawFrame();
        m_Services->BuildWorldBatchKeys();
        std::vector<ViewTargetsRG> targets;
        std::vector<ViewDesc> views;
        RenderGraph::RGTexture color{};
        if (draw)
        {
            TextureDesc desc{};
            desc.width = desc.height = kExtent;
            desc.depth = desc.arrayLayers = desc.mipLevels = desc.sampleCount = 1;
            desc.format = static_cast<uint32_t>(TextureFormat::R16G16B16A16_FLOAT);
            desc.usage = static_cast<uint32_t>(TextureUsage::RenderTarget | TextureUsage::TransferSrc);
            color = frame.ImportPersistentTexture("SkyResting.Color", desc);
            targets.push_back(ViewTargetsRG{m_View, color, {}, {}});
            views.assign(m_Services->Views().GetViews().begin(), m_Services->Views().GetViews().end());
        }
        m_Pipeline->Declare(frame, targets, views);
        const auto lut = frame.FindTexture("Sky_View_LUT");
        ASSERT_TRUE(lut.IsValid());
        if (!draw)
        {
            // Retain the real producer without adding a reader or a final-layout contract.
            frame.MarkOutput(lut);
            return;
        }
        const auto readback = frame.ImportExternalBuffer("SkyResting.ColorReadback", m_Readback, kReadbackBytes);
        frame.AddPass("SkyResting.ReadColor", PassPhase::kFinalize,
            [color, readback](RenderGraph::RGPassBuilder& p) {
                p.Read(color, RenderGraph::RGTextureRead::CopySrc);
                p.Write(readback, RenderGraph::RGBufferWrite::CopyDst);
                p.PreventCulling();
            }, [color, destination = m_Readback](RenderGraph::RGContext& ctx) {
                if (ctx.Cmd)
                    ctx.Cmd->CopyTextureSubresourceToBuffer(ctx.GetTexture(color), 0, 0, destination, kExtent, kExtent);
            });
    }

    void Submit(RenderGraph::RGFrame& frame, bool computeExpected)
    {
        const auto before = frame.SubmittedFrameCount();
        frame.Execute();
        ASSERT_EQ(frame.SubmittedFrameCount(), before + 1);
        CheckComputePasses(frame, computeExpected, true);
        auto& renderer = m_Sky->GetRenderer();
        ASSERT_TRUE(renderer.IsTransmittanceComputed());
        const auto digest = renderer.GetSkyViewLutComputedDigest();
        ASSERT_NE(digest, 0u);
        ASSERT_TRUE(renderer.IsSkyViewLutCurrentFor(digest));
        ASSERT_TRUE(renderer.IsMultiscatterCurrentFor(digest));
    }

    void CheckValidation()
    {
        const auto stats = m_Device->GetValidationStats();
        ASSERT_TRUE(stats.Enabled);
        std::string messages;
        for (const auto& vuid : stats.Vuids)
            messages += vuid.FirstMessage + "\n";
        EXPECT_EQ(stats.ErrorCount, m_ValidationBaseline.ErrorCount) << messages;
        EXPECT_EQ(stats.OverflowCount, m_ValidationBaseline.OverflowCount);
    }

    void CheckColor()
    {
        const auto* pixels = static_cast<const uint16_t*>(m_Device->MapBuffer(m_Readback));
        ASSERT_NE(pixels, nullptr);
        float low = std::numeric_limits<float>::infinity();
        float high = -std::numeric_limits<float>::infinity();
        uint32_t nonFinite = 0;
        for (uint32_t i = 0; i < kExtent * kExtent; ++i)
        {
            float luminance = 0.0f;
            for (uint32_t c = 0; c < 4; ++c)
            {
                const float value = Mathematics::HalfToFloat(pixels[i * 4u + c]);
                nonFinite += std::isfinite(value) ? 0u : 1u;
                if (c < 3)
                    luminance += value;
            }
            low = std::min(low, luminance);
            high = std::max(high, luminance);
        }
        m_Device->UnmapBuffer(m_Readback);
        EXPECT_EQ(nonFinite, 0u);
        EXPECT_GT(high, 0.0f);
        EXPECT_GT(high - low, std::max(1.0e-5f, high * 1.0e-4f));
    }

    TestUtils::ScopedTempDir m_Cache{TestUtils::MakeUniqueTempDirectory("sky_lut_resting_layout")};
    std::unique_ptr<IDevice> m_Device;
    std::unique_ptr<RenderServices> m_Services;
    RenderPipelineNodeRegistry m_Registry;
    std::unique_ptr<RenderPipelineInstance> m_Pipeline;
    SkyRenderFeature* m_Sky = nullptr;
    ViewId m_View{};
    BufferHandle m_Readback{};
    ValidationStats m_ValidationBaseline{};
};

TEST_F(SkyLutRestingLayoutTest, ComputeOnlyFrameThenWarmSamplePreservesRestingLayout)
{
    SkyFramePools pools(m_Device.get());
    RenderGraph::RGFrame frame(m_Device.get(), &pools.Persistent, &pools.Transient, &pools.Uploads);
    const auto physicalLut = m_Sky->GetRenderer().GetSkyViewLutTexture();
    uint64_t digest = 0;
    {
        SkyDeviceFrame deviceFrame(*m_Device);
        ASSERT_TRUE(deviceFrame.Active);
        DeclareFrame(frame, 1, false);
        ASSERT_FALSE(HasFatalFailure());
        CheckComputePasses(frame, true, false);
        Submit(frame, true);
        ASSERT_FALSE(HasFatalFailure());
        deviceFrame.Finish();
        CheckValidation();
        digest = m_Sky->GetRenderer().GetSkyViewLutComputedDigest();
        RecordProperty("producer_final_layout", static_cast<int>(frame.Graph().FinalLayout(frame.FindTexture("Sky_View_LUT").Id)));
    }
    // No LUT observation or manual transition occurs between the two submissions.
    {
        SkyDeviceFrame deviceFrame(*m_Device);
        ASSERT_TRUE(deviceFrame.Active);
        DeclareFrame(frame, 2, true);
        ASSERT_FALSE(HasFatalFailure());
        CheckComputePasses(frame, false, false);
        ASSERT_EQ(m_Sky->GetRenderer().GetSkyViewLutTexture(), physicalLut);
        ASSERT_EQ(m_Sky->GetRenderer().GetSkyViewLutComputedDigest(), digest);
        Submit(frame, false);
        ASSERT_FALSE(HasFatalFailure());
        deviceFrame.Finish();
        CheckColor();
        CheckValidation();
    }
}

TEST_F(SkyLutRestingLayoutTest, ColdComputeAndSampleInSameFrameIsValid)
{
    SkyFramePools pools(m_Device.get());
    RenderGraph::RGFrame frame(m_Device.get(), &pools.Persistent, &pools.Transient, &pools.Uploads);
    SkyDeviceFrame deviceFrame(*m_Device);
    ASSERT_TRUE(deviceFrame.Active);
    DeclareFrame(frame, 1, true);
    ASSERT_FALSE(HasFatalFailure());
    CheckComputePasses(frame, true, false);
    Submit(frame, true);
    ASSERT_FALSE(HasFatalFailure());
    deviceFrame.Finish();
    CheckColor();
    CheckValidation();
}
} // namespace
