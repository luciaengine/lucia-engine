#include <gtest/gtest.h>

#include "Ocean/OceanRasterDepthCapture.h"
#include "Ocean/OceanSeabedDepth.h"
#include "Mathematics/HalfFloat.h"
#include "Engine/Rendering/FrameOrchestrator.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/DrawCommand.h"
#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/GPUScene.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Materials/ShaderCompileService.h"
#include "Rendering/Core/PassPhase.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "TestTempDir.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;
using namespace GameEngine::Ocean;

namespace GameEngine::Ocean
{
struct RasterDepthCaptureTestAccess
{
    static void ReassignReadbacks(OceanRasterDepthCapture& capture, RenderGraph::RGFrame& frame,
                                  std::vector<BufferHandle>& outputs)
    {
        capture.OnFrameAborted(frame);
        const std::vector<float> sentinel(16u * 16u, 1337.0f);
        const auto count = capture.m_Readback.Count();
        ASSERT_GT(count, 0u);
        // A full ring turn replaces every physical slot, including the prepared copy's slot.
        for (uint32 index = 0; index < count; ++index)
        {
            uint64 reservation = 0;
            const auto buffer = capture.m_Readback.ReserveWrite(frame, {0, 0, 16, 16, 200, 16}, reservation);
            ASSERT_TRUE(buffer.IsValid());
            ASSERT_NE(reservation, 0u);
            EXPECT_TRUE(capture.m_Readback.HasPendingWrite(frame, buffer, reservation));
            EXPECT_EQ(std::find(outputs.begin(), outputs.end(), buffer), outputs.end());
            outputs.push_back(buffer);
            capture.m_Device->UpdateBuffer(buffer, 0u, sentinel.size() * sizeof(float), sentinel.data());
        }
    }
};
} // namespace GameEngine::Ocean

namespace
{
struct RasterCaptureProbe
{
    Engine::Renderer::RenderServices& Services;
    OceanRasterDepthCapture& Capture;
    RenderGraph::RGFrame& Frame;
    ViewId MainView;
    bool AllowSubmission = true;
    uint32 Resolution = 16u;

    void Declare(uint64 index, float center, float deep, bool expectConverter, bool abort = false)
    {
        auto* device = Services.GetDevice();
        ASSERT_TRUE(device->BeginFrame());
        Frame.BeginFrame(100u + index * 7u);
        AllowSubmission = !abort;
        Services.BeginWorldDrawFrame();
        Services.BuildWorldBatchKeys();
        Services.ScheduleGpuSkinningAndRetarget(Frame);
        Services.ScheduleViewCullingDispatches(Frame, 1.0f / 60.0f);
        Services.ScheduleWorldBucketerDispatches(Frame);
        const auto view = *Services.Views().FindViewDesc(MainView);
        Engine::Renderer::Pipeline::ViewDeclare d(Frame, view, Services, "OceanRasterCaptureProbe");
        d.RenderWidth = d.RenderHeight = Resolution;
        OceanRasterDepthCaptureSettings settings{};
        settings.Enabled = true;
        settings.Resolution = Resolution;
        settings.SizeX = settings.SizeZ = 16.0f;
        settings.TopPadding = 8.0f;
        settings.DeepWaterDepth = deep;
        EXPECT_TRUE(Capture.DeclareForView(d, center, 0.0f, 0.0f, settings));
        bool converter = false;
        for (RenderGraph::RGPassId pass = 0; pass < Frame.Graph().PassCount(); ++pass)
            converter |= std::strcmp(Frame.Graph().PassName(pass), "Ocean.RasterDepth.Convert") == 0;
        EXPECT_EQ(converter, expectConverter) << "frame " << index;
        if (abort)
            Frame.AddPass("RasterCapture.Abort", PassPhase::kFinalize,
                [this](RenderGraph::RGPassBuilder& pass)
                {
                    pass.PreventCulling();
                    pass.Prepare([this](RenderGraph::RGPrepareContext&) { return AllowSubmission; });
                }, [](RenderGraph::RGContext&) {});
    }

    void Submit(bool abort)
    {
        auto* device = Services.GetDevice();
        const auto submitted = Frame.SubmittedFrameCount();
        Frame.Execute();
        EXPECT_EQ(Frame.SubmittedFrameCount(), submitted + (abort ? 0u : 1u));
        if (abort)
        {
            Capture.OnFrameAborted(Frame);
            Frame.Execute();
            EXPECT_EQ(Frame.SubmittedFrameCount(), submitted);
            Capture.OnFrameAborted(Frame);
        }
        else
            Capture.OnFrameSubmitted(Frame, Frame.SubmissionToken());
        device->FinalizeFrame();
        device->WaitForIdle();
    }

    void Execute(uint64 index, float center, float deep, bool expectConverter, bool abort = false)
    {
        ASSERT_NO_FATAL_FAILURE(Declare(index, center, deep, expectConverter, abort));
        ASSERT_NO_FATAL_FAILURE(Submit(abort));
    }

    void RetryObsolete()
    {
        AllowSubmission = true;
        ASSERT_TRUE(Services.GetDevice()->BeginFrame());
        ASSERT_NO_FATAL_FAILURE(Submit(true));
    }
};

float ReadRasterCentre(IDevice& device, TextureHandle texture, uint32 resolution, bool half = false)
{
    const auto readback = device.CreateReadbackBuffer((half ? sizeof(uint16) : sizeof(float)) * resolution * resolution);
    auto commands = device.CreateCommandList(IDevice::QueueType::Graphics);
    commands->Begin();
    commands->Barrier(ResourceBarrier::CreateTextureBarrier(texture, ResourceState::ShaderResource,
                                                            ResourceState::CopySource));
    commands->CopyTextureToBuffer(texture, readback, resolution, resolution);
    commands->Barrier(ResourceBarrier::CreateTextureBarrier(texture, ResourceState::CopySource,
                                                            ResourceState::ShaderResource));
    commands->End();
    device.ExecuteCommandLists({commands.get()});
    device.WaitForIdle();
    float value = -1.0f;
    if (const auto* mapped = device.MapBuffer(readback))
    {
        const auto centre = (resolution / 2u) * resolution + resolution / 2u;
        value = half ? Mathematics::HalfToFloat(static_cast<const uint16*>(mapped)[centre]) :
                       static_cast<const float*>(mapped)[centre];
        device.UnmapBuffer(readback);
    }
    device.DestroyBuffer(readback);
    return value;
}

void CheckInitialCapture(RasterCaptureProbe& probe)
{
    OceanRasterDepthSampling sampling{};
    ASSERT_NO_FATAL_FAILURE(probe.Execute(0u, 0.0f, 100.0f, false));
    EXPECT_FALSE(probe.Capture.GetSampling(probe.Frame, sampling));
    ASSERT_NO_FATAL_FAILURE(probe.Execute(1u, 0.0f, 100.0f, true));
    EXPECT_FALSE(probe.Capture.GetSampling(probe.Frame, sampling));
    ASSERT_NO_FATAL_FAILURE(probe.Execute(2u, 0.0f, 100.0f, false));
    ASSERT_TRUE(probe.Capture.GetSampling(probe.Frame, sampling));
    float depth = -1.0f;
    ASSERT_TRUE(probe.Capture.SampleDepth(0.0f, 0.0f, depth));
    EXPECT_NEAR(depth, 25.0f, 0.002f);
    EXPECT_NEAR(ReadRasterCentre(*probe.Services.GetDevice(), sampling.Texture, 16u), 25.0f, 0.002f);
}

void CheckAbortRecovery(RasterCaptureProbe& probe, RenderGraph::RGFrame& foreign)
{
    auto& capture = probe.Capture;
    auto& frame = probe.Frame;
    OceanRasterDepthSampling sampling{};
    ASSERT_NO_FATAL_FAILURE(probe.Execute(0u, 0.0f, 100.0f, false));
    EXPECT_FALSE(capture.GetSampling(frame, sampling));
    ASSERT_NO_FATAL_FAILURE(probe.Execute(1u, 0.0f, 100.0f, true));
    EXPECT_FALSE(capture.GetSampling(frame, sampling));
    ASSERT_NO_FATAL_FAILURE(probe.Execute(2u, 0.0f, 100.0f, false));
    EXPECT_TRUE(capture.GetSampling(frame, sampling));
    float depth = -1.0f;
    ASSERT_TRUE(capture.SampleDepth(0.0f, 0.0f, depth));
    EXPECT_NEAR(depth, 25.0f, 0.002f) << "the actual depth draw and converter must reach the CPU tile";
    const auto first = sampling.Texture;
    ASSERT_NO_FATAL_FAILURE(probe.Execute(3u, 1024.0f, 200.0f, false));
    ASSERT_NO_FATAL_FAILURE(probe.Execute(4u, 1024.0f, 200.0f, true, true));
    foreign.BeginFrame(frame.FrameIndex());
    ASSERT_TRUE(capture.GetSampling(foreign, sampling));
    EXPECT_FLOAT_EQ(sampling.OriginX, -8.0f) << "aborted capture cannot advance the recorded footprint";
    EXPECT_EQ(sampling.Texture, first);
    ASSERT_TRUE(capture.SampleDepth(0.0f, 0.0f, depth));
    EXPECT_NEAR(depth, 25.0f, 0.002f);
    ASSERT_NO_FATAL_FAILURE(probe.Execute(5u, 1024.0f, 200.0f, false));
    ASSERT_NO_FATAL_FAILURE(probe.Execute(6u, 1024.0f, 200.0f, true));
    ASSERT_NO_FATAL_FAILURE(probe.Execute(7u, 1024.0f, 200.0f, false));
    EXPECT_TRUE(capture.SampleDepth(1024.0f, 0.0f, depth));
    EXPECT_NEAR(depth, 50.0f, 0.002f) << "unchanged settings after abort must eventually recapture";
}

void CheckSupersededRetry(RasterCaptureProbe& probe, RenderGraph::RGFrame& foreign)
{
    ASSERT_NO_FATAL_FAILURE(CheckInitialCapture(probe));
    ASSERT_NO_FATAL_FAILURE(probe.Execute(3u, 1024.0f, 200.0f, false));
    ASSERT_NO_FATAL_FAILURE(probe.Execute(4u, 1024.0f, 200.0f, true, true));
    OceanRasterDepthSampling planned{};
    ASSERT_TRUE(probe.Capture.GetSampling(probe.Frame, planned));
    EXPECT_FLOAT_EQ(planned.OriginX, 1016.0f);
    RasterCaptureProbe successor{probe.Services, probe.Capture, foreign, probe.MainView};
    ASSERT_NO_FATAL_FAILURE(successor.Execute(5u, 2048.0f, 300.0f, false));
    ASSERT_NO_FATAL_FAILURE(successor.Execute(6u, 2048.0f, 300.0f, true));
    ASSERT_NO_FATAL_FAILURE(successor.Execute(7u, 2048.0f, 300.0f, false));
    OceanRasterDepthSampling committed{};
    ASSERT_TRUE(probe.Capture.GetSampling(foreign, committed));
    EXPECT_EQ(planned.Texture, committed.Texture) << "replacement uses the same uncommitted rotating slot";
    EXPECT_FLOAT_EQ(committed.OriginX, 2040.0f);
    auto& device = *probe.Services.GetDevice();
    EXPECT_NEAR(ReadRasterCentre(device, committed.Texture, 16u), 75.0f, 0.002f);
    ASSERT_NO_FATAL_FAILURE(probe.RetryObsolete());
    EXPECT_NEAR(ReadRasterCentre(device, committed.Texture, 16u), 75.0f, 0.002f)
        << "an obsolete converter must not overwrite the newer capture's pixels";
    float depth = -1.0f;
    ASSERT_TRUE(probe.Capture.SampleDepth(2048.0f, 0.0f, depth));
    EXPECT_NEAR(depth, 75.0f, 0.002f);
}

void CheckInvalidatedRetry(RasterCaptureProbe& probe, RenderGraph::RGFrame& foreign)
{
    ASSERT_NO_FATAL_FAILURE(CheckInitialCapture(probe));
    ASSERT_NO_FATAL_FAILURE(probe.Execute(3u, 1024.0f, 200.0f, false));
    ASSERT_NO_FATAL_FAILURE(probe.Execute(4u, 1024.0f, 200.0f, true, true));
    probe.Capture.InvalidateReadback();
    ASSERT_NO_FATAL_FAILURE(probe.RetryObsolete());
    foreign.BeginFrame(probe.Frame.FrameIndex());
    OceanRasterDepthSampling sampling{};
    EXPECT_FALSE(probe.Capture.GetSampling(foreign, sampling));
    float depth = -1.0f;
    EXPECT_FALSE(probe.Capture.SampleDepth(0.0f, 0.0f, depth));
    EXPECT_FALSE(probe.Capture.SampleDepth(1024.0f, 0.0f, depth));
}

void CheckRevokedReadback(RasterCaptureProbe& probe, RenderGraph::RGFrame&)
{
    ASSERT_NO_FATAL_FAILURE(CheckInitialCapture(probe));
    ASSERT_NO_FATAL_FAILURE(probe.Execute(3u, 1024.0f, 200.0f, false));
    ASSERT_NO_FATAL_FAILURE(probe.Declare(4u, 1024.0f, 200.0f, true));
    RenderGraph::RGPass readback{};
    for (RenderGraph::RGPassId id = 0; id < probe.Frame.Graph().PassCount(); ++id)
        if (std::strcmp(probe.Frame.Graph().PassName(id), "Ocean.RasterDepth.Readback") == 0)
            readback.Id = id;
    EXPECT_TRUE(readback.IsValid());
    std::vector<BufferHandle> replacements;
    const auto revoke = probe.Frame.AddPass("RasterCapture.ReassignPreparedReadback", PassPhase::kWorldRender,
        [](RenderGraph::RGPassBuilder& pass) { pass.PreventCulling(); },
        [&probe, &replacements](RenderGraph::RGContext&)
        {
            RasterDepthCaptureTestAccess::ReassignReadbacks(probe.Capture, probe.Frame, replacements);
        });
    probe.Frame.AddOrderingEdge(revoke, readback);
    ASSERT_NO_FATAL_FAILURE(probe.Submit(false));
    ASSERT_FALSE(replacements.empty());
    auto& device = *probe.Services.GetDevice();
    uint32 overwritten = 0;
    for (const auto buffer : replacements)
    {
        const auto* mapped = static_cast<const float*>(device.MapBuffer(buffer));
        EXPECT_NE(mapped, nullptr);
        if (!mapped)
            continue;
        if (!std::all_of(mapped, mapped + 16u * 16u, [](float value) { return value == 1337.0f; }))
            ++overwritten;
        device.UnmapBuffer(buffer);
    }
    EXPECT_EQ(overwritten, 0u) << "a canceled reservation must not write into its reassigned physical buffer";
    ASSERT_NO_FATAL_FAILURE(probe.Execute(5u, 1024.0f, 200.0f, false));
    ASSERT_NO_FATAL_FAILURE(probe.Execute(6u, 1024.0f, 200.0f, false));
    float depth = -1.0f;
    ASSERT_TRUE(probe.Capture.SampleDepth(1024.0f, 0.0f, depth));
    EXPECT_NEAR(depth, 50.0f, 0.002f) << "the steady footprint retries only the missing copy";
}

void CheckResizedGeneration(RasterCaptureProbe& probe, RenderGraph::RGFrame& foreign)
{
    ASSERT_NO_FATAL_FAILURE(CheckInitialCapture(probe));
    foreign.BeginFrame(probe.Frame.FrameIndex());
    OceanRasterDepthSampling old{};
    ASSERT_TRUE(probe.Capture.GetSampling(foreign, old));
    probe.Resolution = 32u;
    ASSERT_NO_FATAL_FAILURE(probe.Execute(3u, 1024.0f, 200.0f, false));
    ASSERT_NO_FATAL_FAILURE(probe.Execute(4u, 1024.0f, 200.0f, true, true));
    float depth = -1.0f;
    ASSERT_TRUE(probe.Capture.SampleDepth(0.0f, 0.0f, depth));
    EXPECT_NEAR(depth, 25.0f, 0.002f) << "resizing the GPU ring must preserve the CPU tile dimensions";
    OceanRasterDepthSampling afterAbort{};
    ASSERT_TRUE(probe.Capture.GetSampling(foreign, afterAbort));
    EXPECT_EQ(afterAbort.Texture, old.Texture);
    EXPECT_FLOAT_EQ(afterAbort.OriginX, old.OriginX);
    ASSERT_NO_FATAL_FAILURE(probe.Execute(5u, 1024.0f, 200.0f, false));
    ASSERT_NO_FATAL_FAILURE(probe.Execute(6u, 1024.0f, 200.0f, true));
    OceanRasterDepthSampling current{};
    EXPECT_FALSE(probe.Capture.GetSampling(probe.Frame, current));
    for (uint64 index = 7u; index < 12u; ++index)
        ASSERT_NO_FATAL_FAILURE(probe.Execute(index, 1024.0f, 200.0f, false));
    ASSERT_TRUE(probe.Capture.GetSampling(probe.Frame, current));
    EXPECT_NE(current.Texture, old.Texture);
    ASSERT_TRUE(probe.Capture.SampleDepth(1024.0f, 0.0f, depth));
    EXPECT_NEAR(depth, 50.0f, 0.002f);
    EXPECT_NEAR(ReadRasterCentre(*probe.Services.GetDevice(), current.Texture, 32u), 50.0f, 0.002f);
    EXPECT_NEAR(ReadRasterCentre(*probe.Services.GetDevice(), old.Texture, 16u), 25.0f, 0.002f)
        << "the earlier sampling graph must retain the replaced output generation";
}

void CheckForeignSampling(RasterCaptureProbe& probe, RenderGraph::RGFrame& foreign)
{
    ASSERT_NO_FATAL_FAILURE(CheckInitialCapture(probe));
    ASSERT_NO_FATAL_FAILURE(probe.Execute(3u, 1024.0f, 200.0f, false));
    ASSERT_NO_FATAL_FAILURE(probe.Declare(4u, 1024.0f, 200.0f, true, true));
    foreign.BeginFrame(probe.Frame.FrameIndex());
    auto view = *probe.Services.Views().FindViewDesc(probe.MainView);
    view.worldId = 99u;
    view.renderLayerMask = 2u;
    Engine::Renderer::Pipeline::ViewDeclare d(foreign, view, probe.Services, "RasterForeignWindow");
    OceanRasterDepthCaptureSettings settings{};
    settings.Enabled = true;
    settings.Resolution = 32u;
    settings.DeepWaterDepth = 999.0f;
    EXPECT_TRUE(probe.Capture.DeclareForView(d, 2048.0f, 0.0f, 0.0f, settings));
    EXPECT_EQ(foreign.Graph().PassCount(), 0u) << "one world epoch has one capture owner";
    const auto* hidden = probe.Services.Views().FindViewDesc(probe.Capture.GetCaptureViewId());
    EXPECT_EQ(hidden->worldId, 1u);
    EXPECT_EQ(hidden->renderLayerMask, 1u);
    OceanRasterDepthSampling owner{}, other{};
    EXPECT_TRUE(probe.Capture.GetSampling(probe.Frame, owner));
    EXPECT_TRUE(probe.Capture.GetSampling(foreign, other));
    EXPECT_FLOAT_EQ(owner.OriginX, 1016.0f);
    EXPECT_FLOAT_EQ(owner.Deep, 200.0f);
    EXPECT_FLOAT_EQ(other.OriginX, -8.0f);
    EXPECT_FLOAT_EQ(other.Deep, 100.0f);
    EXPECT_NE(owner.Texture, other.Texture);
    ASSERT_NO_FATAL_FAILURE(probe.Submit(true));
    EXPECT_NEAR(ReadRasterCentre(*probe.Services.GetDevice(), other.Texture, 16u), 25.0f, 0.002f);
}

struct RasterRecordWitness
{
    uint32 Descriptors = 0;
    bool Before = false, After = false;

    void Declare(RenderGraph::RGFrame& frame, IDevice& device)
    {
        const auto count = frame.Graph().PassCount();
        RenderGraph::RGPass converter{}, readback{};
        for (RenderGraph::RGPassId id = 0; id < count; ++id)
        {
            if (std::strcmp(frame.Graph().PassName(id), "Ocean.RasterDepth.Convert") == 0)
                converter.Id = id;
            if (std::strcmp(frame.Graph().PassName(id), "Ocean.RasterDepth.Readback") == 0)
                readback.Id = id;
        }
        ASSERT_TRUE(converter.IsValid() && readback.IsValid());
        const auto before = frame.AddPass("RasterCapture.BeforeConvert", PassPhase::kWorldRender,
            [](RenderGraph::RGPassBuilder& pass) { pass.PreventCulling(); },
            [this, &device](RenderGraph::RGContext&)
            {
                Before = true;
                if (device.GetAPI() == GraphicsAPI::Vulkan)
                {
                    Descriptors = device.GetDescriptorAllocatorStats().TransientAllocated;
                    EXPECT_GT(Descriptors, 0u);
                }
            });
        for (RenderGraph::RGPassId id = 0; id < count; ++id)
            if (id != converter.Id && id != readback.Id)
                frame.AddOrderingEdge(RenderGraph::RGPass{id}, before);
        const auto after = frame.AddPass("RasterCapture.AfterConvert", PassPhase::kWorldRender,
            [](RenderGraph::RGPassBuilder& pass) { pass.PreventCulling(); },
            [this, &device](RenderGraph::RGContext&)
            {
                After = true;
                if (device.GetAPI() == GraphicsAPI::Vulkan)
                {
                    const auto count = device.GetDescriptorAllocatorStats().TransientAllocated;
                    ::testing::Test::RecordProperty("DescriptorsBeforeRaster", Descriptors);
                    ::testing::Test::RecordProperty("DescriptorsAfterRaster", count);
                    EXPECT_EQ(count, Descriptors);
                }
            });
        frame.AddOrderingEdge(before, converter);
        frame.AddOrderingEdge(converter, after);
        frame.AddOrderingEdge(after, readback);
    }
};

void CheckPreparedBindings(RasterCaptureProbe& probe, RenderGraph::RGFrame&)
{
    ASSERT_NO_FATAL_FAILURE(probe.Execute(0u, 0.0f, 100.0f, false));
    ASSERT_NO_FATAL_FAILURE(probe.Declare(1u, 0.0f, 100.0f, true, true));
    RasterRecordWitness witness{};
    ASSERT_NO_FATAL_FAILURE(witness.Declare(probe.Frame, *probe.Services.GetDevice()));
    ASSERT_NO_FATAL_FAILURE(probe.Submit(true));
    EXPECT_FALSE(witness.Before);
    EXPECT_FALSE(witness.After);
    probe.AllowSubmission = true;
    ASSERT_TRUE(probe.Services.GetDevice()->BeginFrame());
    ASSERT_NO_FATAL_FAILURE(probe.Submit(false));
    EXPECT_TRUE(witness.Before);
    EXPECT_TRUE(witness.After);
    ASSERT_NO_FATAL_FAILURE(probe.Execute(2u, 0.0f, 100.0f, false));
    float depth = -1.0f;
    ASSERT_TRUE(probe.Capture.SampleDepth(0.0f, 0.0f, depth));
    EXPECT_NEAR(depth, 25.0f, 0.002f);
}

void CheckSeabedSnapshot(RasterCaptureProbe& probe, RenderGraph::RGFrame& foreign)
{
    ASSERT_NO_FATAL_FAILURE(CheckInitialCapture(probe));
    OceanRasterDepthSampling first{}, second{};
    ASSERT_TRUE(probe.Capture.GetSampling(probe.Frame, first));
    ASSERT_NO_FATAL_FAILURE(probe.Execute(3u, 0.0f, 200.0f, false));
    ASSERT_NO_FATAL_FAILURE(probe.Execute(4u, 0.0f, 200.0f, true));
    ASSERT_NO_FATAL_FAILURE(probe.Execute(5u, 0.0f, 200.0f, false));
    ASSERT_TRUE(probe.Capture.GetSampling(probe.Frame, second));
    auto& device = *probe.Services.GetDevice();
    ASSERT_NE(first.Texture, second.Texture);
    EXPECT_NEAR(ReadRasterCentre(device, first.Texture, 16u), 25.0f, 0.002f);
    EXPECT_NEAR(ReadRasterCentre(device, second.Texture, 16u), 50.0f, 0.002f);
    OceanSeabedDepth seabed;
    ASSERT_TRUE(seabed.Initialize(&device));
    seabed.ConfigureCascades(&device, 16.0f, 16u);
    seabed.SetRasterDepthCapture(first.Texture, first.Sampler, first.OriginX, first.OriginZ,
                                first.SizeX, first.SizeZ, first.Deep);
    ASSERT_TRUE(seabed.BeginFrame(0.0f, 0.0f));
    ASSERT_TRUE(device.BeginFrame());
    foreign.BeginFrame(999u);
    const auto params = foreign.AllocUpload<OceanSeabedDepthParamsGPU>();
    EXPECT_TRUE(params.Valid());
    seabed.FillParams(*params.Ptr);
    EXPECT_EQ(params.Ptr->RasterDepthCaptureAvailable, 1u);
    const auto target = seabed.ImportRG(foreign);
    const auto input = foreign.ImportExternalTexture("RasterSnapshot.Input", first.Texture,
        ResourceState::ShaderResource, TextureFormat::R32_FLOAT);
    auto* prepared = foreign.AllocatePassData<OceanPreparedDispatch>();
    foreign.AddPass("RasterSnapshot.Seabed", PassPhase::kEarlySetup,
        [&seabed, prepared, params, first, target, input](RenderGraph::RGPassBuilder& pass)
        {
            pass.UsePipeline(seabed.GetPipelineId());
            pass.Write(target, RenderGraph::RGTextureWrite::Storage);
            pass.Read(input, RenderGraph::RGTextureRead::SampledCompute);
            pass.Prepare([&seabed, prepared, params, first](RenderGraph::RGPrepareContext& ctx)
            {
                *prepared = seabed.PrepareDispatch(ctx.GetDevice(), params.Buffer, params.Offset,
                                                   first.Texture, first.Sampler);
                return prepared->IsValid();
            });
        },
        [&seabed, prepared](RenderGraph::RGContext& ctx) { seabed.RecordDispatch(ctx.Cmd, *prepared); });
    foreign.MarkOutput(target, RenderGraph::RGImageLayout::ShaderReadOnly);
    seabed.SetRasterDepthCapture(second.Texture, second.Sampler, second.OriginX, second.OriginZ,
                                second.SizeX, second.SizeZ, second.Deep);
    const auto submitted = foreign.SubmittedFrameCount();
    foreign.Execute();
    EXPECT_EQ(foreign.SubmittedFrameCount(), submitted + 1u);
    device.FinalizeFrame();
    device.WaitForIdle();
    EXPECT_NEAR(ReadRasterCentre(device, seabed.GetDepthTexture(), 16u, true), 25.0f, 0.01f)
        << "seabed descriptors must retain the raster texture paired with the captured parameters";
}

class RasterCapturePreparationTest : public ::testing::Test
{
  protected:
    virtual GraphicsAPI RequestedAPI() const { return GraphicsAPI::Vulkan; }
    void SetUp() override
    {
        DeviceDesc desc{};
        desc.preferredAPI = RequestedAPI();
        desc.enableSwapchain = false;
        desc.enableDebugLayer = false;
        desc.descriptorBuffers = DescriptorBufferMode::Disabled;
        desc.pipelineCacheDirectory = m_Cache.Path().string();
        m_Device = DeviceFactory::CreateDevice(desc);
        if (!m_Device || !m_Device->Initialize(desc))
            GTEST_SKIP() << "Requested ocean readback backend unavailable";
        ASSERT_EQ(m_Device->GetAPI(), RequestedAPI());
    }
    void TearDown() override
    {
        if (!m_Device)
            return;
        m_Device->WaitForIdle();
        for (auto buffer : m_Buffers)
            m_Device->DestroyBuffer(buffer);
        for (auto texture : m_Textures)
            m_Device->DestroyTexture(texture);
        for (auto sampler : m_Samplers)
            m_Device->DestroySampler(sampler);
        m_Device->Shutdown();
    }

    void CheckRasterCapture(void (*check)(RasterCaptureProbe&, RenderGraph::RGFrame&))
    {
        ASSERT_EQ(m_Device->GetAPI(), RequestedAPI());
        Engine::Renderer::RenderServices services;
        ASSERT_TRUE(services.Initialize(m_Device.get()));
        const auto cameraId = services.Views().AllocateCamera("RasterCaptureProbe.Camera");
        CameraData camera{};
        for (int index = 0; index < 16; index += 5)
            camera.view[index] = camera.proj[index] = camera.viewProj[index] = 1.0f;
        services.Views().SetCameraData(cameraId, camera);
        const auto view = services.Views().AllocateView("RasterCaptureProbe.Main", cameraId);
        services.Views().SetViewWorldId(view, 1u);
        services.Views().SetViewRenderLayerMask(view, 1u);
        GPUInstance instance{};
        instance.meshIndex = services.GetGPUScene()->AddMesh(GPUMesh{});
        instance.boundingRadius = 1.0f;
        services.GetGPUScene()->AddInstance(instance);
        ShaderProgramCompileRequest request{};
        request.debugName = "RasterCaptureSource";
        request.baseDirectory = m_Cache.Path();
        request.cacheRoot = m_Cache.Path() / "ShaderCache";
        ShaderStageCompileSpec vertex{};
        vertex.stage = "vs";
        vertex.sourcePath = m_Cache.Path() / "raster_source.vert";
        vertex.inlineSource = R"glsl(#version 450
void main()
{
    vec2 positions[3] = vec2[](vec2(-1,-1), vec2(3,-1), vec2(-1,3));
    gl_Position = vec4(positions[gl_VertexIndex], 0.75, 1.0);
}
)glsl";
        ShaderStageCompileSpec fragment{};
        fragment.stage = "fs";
        fragment.sourcePath = m_Cache.Path() / "raster_source.frag";
        fragment.inlineSource = R"glsl(#version 450
layout(location = 0) out vec4 color;
void main() { color = vec4(1); }
)glsl";
        request.stages = {vertex, fragment};
        ShaderProgramCompileResult program{};
        std::string error;
        ASSERT_TRUE(ShaderCompileService::CompileProgramToCache(request, ShaderSourceKind::SpirV, program, &error)) << error;
        ASSERT_TRUE(program.stageBytes.contains("vs") && program.stageBytes.contains("fs"));
        GraphicsPipelineDesc pipeline{};
        pipeline.VertexShader = std::make_shared<const std::vector<uint8_t>>(program.stageBytes.at("vs"));
        pipeline.PixelShader = std::make_shared<const std::vector<uint8_t>>(program.stageBytes.at("fs"));
        pipeline.Rasterization.cullMode = CullModeFlagBits::None;
        pipeline.DepthStencil.depthTestEnable = true;
        pipeline.DepthStencil.depthWriteEnable = true;
        pipeline.DepthStencil.depthCompareOp = CompareOp::Always;
        pipeline.ColorBlend.attachments.emplace_back();
        pipeline.DebugName = "RasterCaptureSource";
        const auto sourcePipeline = m_Device->InternGraphicsPipeline(pipeline);
        PipelineFormatKey format{};
        format.ColorCount = 1u;
        format.ColorFormats[0] = TextureFormat::RGBA8_UNORM;
        format.DepthFormat = services.GetDepthFormat();
        ASSERT_TRUE(m_Device->GetOrCreateGraphicsPipeline(sourcePipeline, format).IsValid());
        const std::array<uint16_t, 3> indices{0u, 1u, 2u};
        BufferDesc indexDesc{};
        indexDesc.size = sizeof(indices);
        indexDesc.usage = static_cast<uint32>(BufferUsage::Index);
        indexDesc.memoryUsage = BufferMemoryUsage::Upload;
        const auto indexBuffer = m_Device->CreateBuffer(indexDesc);
        ASSERT_TRUE(indexBuffer.IsValid());
        m_Buffers.push_back(indexBuffer);
        m_Device->UpdateBuffer(indexBuffer, 0u, sizeof(indices), indices.data());
        auto sourceMaterial = Engine::Renderer::Material::TestFactory::Create(GUID::Generate(), "RasterCaptureSource", 16u);
        Engine::Renderer::Material::TestFactory::SetShaderMeta(sourceMaterial, std::make_shared<ShaderMeta>(program.meta));
        {
            OceanRasterDepthCapture capture;
            auto producer = services.RegisterForwardEmit(
                [&](Engine::Renderer::ForwardEmitContext& ctx)
                {
                    if (ctx.ViewId != capture.GetCaptureViewId())
                        return;
                    Engine::Renderer::DrawCommand command{};
                    command.InternedPipeline = sourcePipeline;
                    command.Material = &sourceMaterial;
                    command.PipelineMeta = &program.meta;
                    command.Geometry.AltGeom.IB = indexBuffer;
                    command.Geometry.AltGeom.IndexType = IndexType::Uint16;
                    command.IndexCount = 3u;
                    ctx.Services->EmitForwardCommand(ctx.ViewId, command, Engine::Renderer::ForwardDrawDepth::ColourPass);
                });
            RenderGraph::RGResourcePool persistent(m_Device.get());
            RenderGraph::RGTransientPool transient(m_Device.get());
            RenderGraph::RGUploadRing ring(m_Device.get(), 3u, 262144u);
            RenderGraph::RGFrame frame(m_Device.get(), &persistent, &transient, &ring);
            RasterCaptureProbe probe{services, capture, frame, view};
            RenderGraph::RGResourcePool foreignPersistent(m_Device.get());
            RenderGraph::RGTransientPool foreignTransient(m_Device.get());
            RenderGraph::RGUploadRing foreignRing(m_Device.get(), 3u, 262144u);
            RenderGraph::RGFrame foreign(m_Device.get(), &foreignPersistent, &foreignTransient, &foreignRing);
            ASSERT_NO_FATAL_FAILURE(check(probe, foreign));
        }
        services.Shutdown();
    }

    std::unique_ptr<IDevice> m_Device;
    std::vector<BufferHandle> m_Buffers;
    std::vector<TextureHandle> m_Textures;
    std::vector<SamplerHandle> m_Samplers;
    TestUtils::ScopedTempDir m_Cache{TestUtils::MakeUniqueTempDirectory("ocean_raster_preparation")};
};
} // namespace

TEST_F(RasterCapturePreparationTest, AbortDoesNotConsumeItsFootprint)
{
    CheckRasterCapture(CheckAbortRecovery);
}
TEST_F(RasterCapturePreparationTest, SupersededRetryCannotOverwriteTheNewerCapture)
{
    CheckRasterCapture(CheckSupersededRetry);
}
TEST_F(RasterCapturePreparationTest, RevokedReadbackCannotOverwriteAReassignedSlot)
{
    CheckRasterCapture(CheckRevokedReadback);
}
TEST_F(RasterCapturePreparationTest, ResizeRetainsTheOldSamplingGeneration)
{
    CheckRasterCapture(CheckResizedGeneration);
}
TEST_F(RasterCapturePreparationTest, ForeignWindowUsesCommittedSampling)
{
    CheckRasterCapture(CheckForeignSampling);
}
TEST_F(RasterCapturePreparationTest, BindingsAreReadyBeforeRecordingAfterRetry)
{
    CheckRasterCapture(CheckPreparedBindings);
}
TEST_F(RasterCapturePreparationTest, SeabedUsesTheDeclaredRasterSnapshot)
{
    CheckRasterCapture(CheckSeabedSnapshot);
}
TEST_F(RasterCapturePreparationTest, InvalidationRejectsTheAbortedCapture)
{
    CheckRasterCapture(CheckInvalidatedRetry);
}
#if defined(__APPLE__)
class MetalRasterCapturePreparationTest : public RasterCapturePreparationTest
{
  protected:
    GraphicsAPI RequestedAPI() const override { return GraphicsAPI::Metal; }
};
TEST_F(MetalRasterCapturePreparationTest, AbortDoesNotConsumeItsFootprint)
{
    CheckRasterCapture(CheckAbortRecovery);
}
TEST_F(MetalRasterCapturePreparationTest, SupersededRetryCannotOverwriteTheNewerCapture)
{
    CheckRasterCapture(CheckSupersededRetry);
}
TEST_F(MetalRasterCapturePreparationTest, RevokedReadbackCannotOverwriteAReassignedSlot)
{
    CheckRasterCapture(CheckRevokedReadback);
}
TEST_F(MetalRasterCapturePreparationTest, ResizeRetainsTheOldSamplingGeneration)
{
    CheckRasterCapture(CheckResizedGeneration);
}
TEST_F(MetalRasterCapturePreparationTest, ForeignWindowUsesCommittedSampling)
{
    CheckRasterCapture(CheckForeignSampling);
}
TEST_F(MetalRasterCapturePreparationTest, BindingsAreReadyBeforeRecordingAfterRetry)
{
    CheckRasterCapture(CheckPreparedBindings);
}
TEST_F(MetalRasterCapturePreparationTest, SeabedUsesTheDeclaredRasterSnapshot)
{
    CheckRasterCapture(CheckSeabedSnapshot);
}
TEST_F(MetalRasterCapturePreparationTest, InvalidationRejectsTheAbortedCapture)
{
    CheckRasterCapture(CheckInvalidatedRetry);
}
#endif
