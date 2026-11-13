#include <gtest/gtest.h>

#include "Components/Rendering/Light.h"
#include "Engine/Rendering/Pipeline/Nodes/ShadowMapNode.h"
#include "Engine/Rendering/Pipeline/RenderPipeline.h"
#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"
#include "Engine/Rendering/RenderServices.h"
#include "Ocean/OceanFrameStamp.h"
#include "Ocean/OceanShadowSim.h"
#include "Ocean/OceanTypes.h"
#include "Rendering/Camera/CameraTypes.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "TestTempDir.h"
#include "TestCameraEye.h"
#include "TestDeviceHelper.h"

#include <cmath>
#include <cstring>
#include <memory>
#include <set>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;
using namespace GameEngine::Engine::Renderer;
using namespace GameEngine::Engine::Renderer::Pipeline;
using namespace GameEngine::Ocean;

namespace
{
// Three frames in flight on every backend: seven frames wrap the device slot twice.
constexpr uint32_t kFramesAcrossTwoSlotWraps = 7u;
constexpr uint32_t kTargetExtent = 64u;
constexpr uint32_t kShadowResolution = 32u;
// An authored shadow covers the field until this graph frame, which lands on a
// device-slot wrap: the first frame whose slot is lower than its predecessor's.
constexpr uint64 kShadowRemovedFrame = 3u;
// With history the field keeps most of the removed shadow for a frame; without it
// the field is fully lit at once.
constexpr float kLitWithoutHistory = 0.5f;

float HalfToFloat(uint16_t half)
{
    const uint32_t sign = (half >> 15) & 1u, exponent = (half >> 10) & 0x1fu, mantissa = half & 0x3ffu;
    const float magnitude = exponent == 0u ? std::ldexp(static_cast<float>(mantissa), -24)
                                           : std::ldexp(static_cast<float>(mantissa | 0x400u),
                                                        static_cast<int>(exponent) - 25);
    return sign ? -magnitude : magnitude;
}

// The hard-shadow visibility at the centre texel of cascade 0 (R16G16F: lit
// hard, lit soft).
float ReadCentreVisibility(IDevice& device, TextureHandle field, uint32 resolution = kShadowResolution)
{
    const size_t bytes = size_t(resolution) * resolution * 4u;
    const BufferHandle readback = device.CreateReadbackBuffer(bytes);
    auto commands = device.CreateCommandList(IDevice::QueueType::Graphics);
    commands->Begin();
    commands->Barrier(ResourceBarrier::CreateTextureBarrier(field, ResourceState::ShaderResource,
                                                            ResourceState::CopySource));
    commands->CopyTextureToBuffer(field, readback, resolution, resolution);
    commands->Barrier(ResourceBarrier::CreateTextureBarrier(field, ResourceState::CopySource,
                                                            ResourceState::ShaderResource));
    commands->End();
    device.ExecuteCommandLists({commands.get()});
    device.WaitForIdle();
    float visibility = -1.0f;
    if (const auto* mapped = static_cast<const uint8_t*>(device.MapBuffer(readback)))
    {
        const size_t centre = (size_t(resolution / 2u) * resolution + resolution / 2u) * 4u;
        uint16_t half = 0;
        std::memcpy(&half, mapped + centre, 2u);
        visibility = HalfToFloat(half);
        device.UnmapBuffer(readback);
    }
    device.DestroyBuffer(readback);
    return visibility;
}

struct FramePools
{
    RenderGraph::RGResourcePool Persistent;
    RenderGraph::RGTransientPool Transient;
    RenderGraph::RGUploadRing Ring;
    explicit FramePools(IDevice* device) : Persistent(device), Transient(device), Ring(device, 2, 262144) {}
};

TextureDesc TargetDesc(TextureFormat format, uint32_t usage)
{
    TextureDesc desc{};
    desc.width = kTargetExtent;
    desc.height = kTargetExtent;
    desc.depth = 1;
    desc.mipLevels = 1;
    desc.arrayLayers = 1;
    desc.sampleCount = 1;
    desc.format = static_cast<uint32_t>(format);
    desc.usage = usage;
    return desc;
}

// Declares the ocean's temporal shadow accumulation after the engine shadow
// pass, as the ocean node does, and records whether it declared.
struct OceanShadowProbeNode final : IRenderPipelineNode
{
    static OceanShadowSim* s_Sim;
    static bool s_Declared;

    const char* GetTypeName() const override { return "OceanShadowProbe"; }
    bool Initialize(std::string, std::string, std::string*) override { return true; }
    void DeclareForView(ViewDeclare& d) override
    {
        OceanParamsGPU params{};
        s_Declared = s_Sim && s_Sim->DeclareForView(d, params);
    }
};
OceanShadowSim* OceanShadowProbeNode::s_Sim = nullptr;
bool OceanShadowProbeNode::s_Declared = false;

RenderPipelineBlueprint::Pass MakePass(const char* id, const char* type, const char* json)
{
    RenderPipelineBlueprint::Pass pass;
    pass.id = id;
    pass.type = type;
    pass.enabled = true;
    pass.perView = true;
    pass.passJson = json;
    return pass;
}
struct ShadowViewDeclaration
{
    RenderServices& Services;
    RenderPipelineInstance& Pipeline;
    OceanShadowSim& Sim;
    CameraData& Data;
    CameraId Camera;
    ViewId View;

    void Declare(RenderGraph::RGFrame& target, uint64 index, float cameraX,
                 bool authored, bool advanceEpoch = true)
    {
        target.BeginFrame(index);
        Data.cameraPos[0] = cameraX;
        Services.Views().SetCameraData(Camera, ::GameEngine::Tests::TestCameraWithEye(Data), {0.1f, 1000.0f});
        const auto color = target.ImportPersistentTexture("OceanShadow.Color",
            TargetDesc(TextureFormat::RGBA8_UNORM, static_cast<uint32_t>(TextureUsage::RenderTarget)));
        const auto depth = target.ImportPersistentTexture("OceanShadow.Depth",
            TargetDesc(TextureFormat::D32_FLOAT,
                       static_cast<uint32_t>(TextureUsage::DepthStencil | TextureUsage::ShaderResource)));
        if (advanceEpoch)
            Services.BeginWorldDrawFrame();
        Services.BuildWorldBatchKeys();
        ExtractedLight sun{};
        sun.type = GameEngine::Components::LightType::Directional;
        sun.castsShadows = sun.castsLight = 1;
        sun.directionWS[1] = -1.0f;
        sun.cascadeCount = 1;
        Services.SubmitLight(0u, sun);
        std::vector<OceanInputDrawPacket> inputs;
        if (authored)
        {
            OceanInputDrawPacket input{};
            input.Family = OceanInputFamily::Shadow;
            input.CenterX = cameraX;
            input.ExtentX = input.ExtentZ = 1000.0f;
            input.Value[0] = input.Value[1] = 1.0f;
            inputs.push_back(input);
        }
        Sim.SetInputs(std::move(inputs));
        OceanShadowProbeNode::s_Declared = false;
        const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{View, color, depth, {}}};
        const std::vector<ViewDesc> views(Services.Views().GetViews().begin(), Services.Views().GetViews().end());
        Pipeline.Declare(target, targets, views);
        target.MarkOutput(color);
    }
};
} // namespace

TEST(OceanFrameStamp, ConsecutiveGraphFramesContinueAcrossDeviceSlotWraps)
{
    OceanFrameStamp stamp;
    EXPECT_FALSE(stamp.PrecedesFrame(0u)) << "a view that never declared has no history";
    for (uint64 frame = 0; frame < kFramesAcrossTwoSlotWraps; ++frame)
    {
        if (frame > 0u)
            EXPECT_TRUE(stamp.PrecedesFrame(frame)) << "frame " << frame;
        EXPECT_FALSE(stamp.IsFrame(frame));
        stamp.Frame = frame;
        EXPECT_TRUE(stamp.IsFrame(frame));
        EXPECT_FALSE(stamp.PrecedesFrame(frame)) << "the frame itself is not its predecessor";
    }
    EXPECT_FALSE(stamp.PrecedesFrame(kFramesAcrossTwoSlotWraps + 1u)) << "a skipped frame breaks history";
}

// The ocean shadow history is identified by the render graph's frame index,
// not by the device slot: across two slot wraps every frame declares, and the
// surface finds this frame's field under the index the graph was begun with.
namespace
{
void CheckShadowHistory(GraphicsAPI api, bool checkRecovery)
{
    TestUtils::ScopedTempDir cache{TestUtils::MakeUniqueTempDirectory("ocean_shadow_preparation")};
    DeviceDesc desc{};
    desc.preferredAPI = api;
    desc.enableSwapchain = false;
    desc.enableDebugLayer = false;
    desc.pipelineCacheDirectory = cache.Path().string();
    if (checkRecovery && api == GraphicsAPI::Vulkan)
        desc.descriptorBuffers = DescriptorBufferMode::Disabled;
    auto device = DeviceFactory::CreateDevice(desc);
    if (device && !device->Initialize(desc))
        device.reset();
    if (!device)
        GTEST_SKIP() << "Requested ocean shadow backend unavailable";
    ASSERT_EQ(device->GetAPI(), api);
    {
        RenderServices rs;
        ASSERT_TRUE(rs.Initialize(device.get()));
        const CameraId cameraId = rs.Views().AllocateCamera("OceanShadowCamera");
        CameraData camera{};
        camera.proj[0][0] = 1.0f;
        camera.proj[1][1] = 1.0f;
        camera.proj[2][2] = 0.001f;
        camera.proj[2][3] = 1.0f;
        camera.proj[3][2] = 0.1f;
        camera.view = Mathematics::Matrix4x4::Identity();
        camera.viewProj = Mathematics::Matrix4x4::Identity();
        rs.Views().SetCameraData(cameraId, ::GameEngine::Tests::TestCameraWithEye(camera), {0.1f, 1000.0f});
        const ViewId viewId = rs.Views().AllocateView("OceanShadowView", cameraId);
        rs.Views().SetViewRenderLayerMask(viewId, Rendering::RenderLayerMask::Default());
        ViewClearConfig clear{};
        clear.clearColor = true;
        clear.clearDepth = true;
        rs.Views().SetViewTargets(viewId, 0, 0, 0, clear);
        auto depthEmit = rs.RegisterDepthEmit(
            [&](DepthEmitContext& ctx, DepthPassType passType)
            {
                if (ctx.ViewId == viewId && passType == DepthPassType::ShadowCascade)
                    ctx.Services->EmitDepthCommand(ctx.ViewId, passType, DrawCommand{});
            });

        RenderPipelineNodeRegistry registry;
        ASSERT_TRUE(registry.Register(
            "ShadowMap", [] { return std::make_unique<Nodes::ShadowMapNode>(); }, true));
        ASSERT_TRUE(registry.Register(
            "OceanShadowProbe", [] { return std::make_unique<OceanShadowProbeNode>(); }, true));
        RenderPipelineBlueprint blueprint;
        blueprint.pipelineName = "OceanShadowTest";
        blueprint.passes.push_back(
            MakePass("CSM", "ShadowMap", R"({"id":"CSM","type":"ShadowMap","buffer":"ShadowData"})"));
        blueprint.passes.push_back(
            MakePass("Ocean", "OceanShadowProbe", R"({"id":"Ocean","type":"OceanShadowProbe"})"));
        RenderPipelineInstance instance(rs, registry);
        instance.SetBlueprint(blueprint);

        OceanShadowSim sim;
        OceanShadowSettings settings{};
        settings.Enabled = true;
        settings.Resolution = kShadowResolution;
        settings.TemporalWeight = 0.95f;
        settings.SimulationFrequency = 60.0f;
        settings.CascadeCount = 1u;
        sim.SetSettings(settings);
        OceanShadowProbeNode::s_Sim = &sim;

        FramePools pools(device.get());
        RenderGraph::RGFrame frame(device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
        std::set<uint32_t> slotsSeen;
        for (uint64 index = 0; index < kFramesAcrossTwoSlotWraps; ++index)
        {
            ASSERT_TRUE(device->BeginFrame());
            slotsSeen.insert(device->GetFrameIndex());
            frame.BeginFrame(index);
            const auto color = frame.ImportPersistentTexture(
                "OceanShadow.Color",
                TargetDesc(TextureFormat::RGBA8_UNORM, static_cast<uint32_t>(TextureUsage::RenderTarget)));
            const auto depth = frame.ImportPersistentTexture(
                "OceanShadow.Depth",
                TargetDesc(TextureFormat::D32_FLOAT, static_cast<uint32_t>(TextureUsage::DepthStencil |
                                                                           TextureUsage::ShaderResource)));
            rs.BeginWorldDrawFrame();
            rs.BuildWorldBatchKeys();
            ExtractedLight sun{};
            sun.type = GameEngine::Components::LightType::Directional;
            sun.castsShadows = 1;
            sun.castsLight = 1;
            sun.directionWS[1] = -1.0f;
            sun.cascadeCount = 1;
            rs.SubmitLight(0u, sun);

            OceanShadowProbeNode::s_Declared = false;
            const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{viewId, color, depth, {}}};
            const std::vector<ViewDesc> views(rs.Views().GetViews().begin(), rs.Views().GetViews().end());
            // A full shadow over the field for the first frames, then none: from
            // the removal frame on, only history carries the shadow forward.
            std::vector<OceanInputDrawPacket> shadow;
            if (index < kShadowRemovedFrame)
            {
                OceanInputDrawPacket packet{};
                packet.Family = OceanInputFamily::Shadow;
                packet.ExtentX = 1000.0f;
                packet.ExtentZ = 1000.0f;
                packet.Value[0] = 1.0f;
                packet.Value[1] = 1.0f;
                shadow.push_back(packet);
            }
            sim.SetInputs(std::move(shadow));
            instance.Declare(frame, targets, views);
            ASSERT_TRUE(OceanShadowProbeNode::s_Declared) << "graph frame " << index;
            frame.MarkOutput(sim.ImportRG(frame, viewId), RenderGraph::RGImageLayout::ShaderReadOnly);

            OceanShadowSamplingGPU sampling{};
            EXPECT_TRUE(sim.FillSampling(viewId, frame, sampling))
                << "graph frame " << index << " on device slot " << device->GetFrameIndex();
            EXPECT_EQ(sampling.Channels[2], 1.0f);

            frame.MarkOutput(color);
            uint32 descriptorCount = 0;
            bool observedBefore = false, observedAfter = false;
            if (checkRecovery && api == GraphicsAPI::Vulkan && index == 0u)
            {
                const auto count = frame.Graph().PassCount();
                RenderGraph::RGPass writer{};
                for (RenderGraph::RGPassId id = 0; id < count; ++id)
                    if (std::strcmp(frame.Graph().PassName(id), "OceanShadowAccumulate") == 0)
                        writer.Id = id;
                ASSERT_TRUE(writer.IsValid());
                const auto observer = frame.AddPass("OceanShadow.BeforeRecord", PassPhase::kEarlySetup,
                    [](RenderGraph::RGPassBuilder& pass) { pass.PreventCulling(); },
                    [&](RenderGraph::RGContext&)
                    {
                        descriptorCount = device->GetDescriptorAllocatorStats().TransientAllocated;
                        EXPECT_GT(descriptorCount, 0u) << "the pool counter must observe actual allocations";
                        observedBefore = true;
                    });
                for (RenderGraph::RGPassId id = 0; id < count; ++id)
                    if (id != writer.Id)
                        frame.AddOrderingEdge(RenderGraph::RGPass{id}, observer);
                const auto after = frame.AddPass("OceanShadow.AfterRecord", PassPhase::kEarlySetup,
                    [](RenderGraph::RGPassBuilder& pass) { pass.PreventCulling(); },
                    [&](RenderGraph::RGContext&)
                    {
                        const auto afterCount = device->GetDescriptorAllocatorStats().TransientAllocated;
                        ::testing::Test::RecordProperty("DescriptorsBeforeShadow", descriptorCount);
                        ::testing::Test::RecordProperty("DescriptorsAfterShadow", afterCount);
                        EXPECT_EQ(afterCount, descriptorCount)
                            << "the shadow record callback must use its prepared descriptor set";
                        observedAfter = true;
                    });
                frame.AddOrderingEdge(observer, writer);
                frame.AddOrderingEdge(writer, after);
            }
            frame.Execute();
            if (checkRecovery && api == GraphicsAPI::Vulkan && index == 0u)
            {
                EXPECT_TRUE(observedBefore);
                EXPECT_TRUE(observedAfter);
            }
            device->FinalizeFrame();
            device->WaitForIdle();
            if (index + 1u == kShadowRemovedFrame)
                EXPECT_LT(ReadCentreVisibility(*device, sim.GetTexture(viewId)), 0.05f)
                    << "the authored shadow must reach the field before the history check means anything";
            if (index == kShadowRemovedFrame)
                EXPECT_LT(ReadCentreVisibility(*device, sim.GetTexture(viewId)), kLitWithoutHistory)
                    << "history must carry the shadow across the device-slot wrap at graph frame " << index;
        }
        EXPECT_EQ(slotsSeen.size(), device->GetFramesInFlight())
            << "the test must drive the device slot through a wrap to mean anything";
        EXPECT_LT(slotsSeen.size(), kFramesAcrossTwoSlotWraps);

        if (checkRecovery)
        {
            // Different windows can have the same numeric graph index.
            FramePools otherPools(device.get());
            RenderGraph::RGFrame other(device.get(), &otherPools.Persistent, &otherPools.Transient, &otherPools.Ring);
            ShadowViewDeclaration declaration{rs, instance, sim, camera, cameraId, viewId};
            const auto recorded = sim.GetTexture(viewId);
            ASSERT_TRUE(recorded.IsValid());
            ASSERT_TRUE(device->BeginFrame());
            declaration.Declare(frame, 7u, 1024.0f, true);
            ASSERT_TRUE(OceanShadowProbeNode::s_Declared);
            const auto pending = sim.GetTexture(viewId, frame);
            EXPECT_NE(pending, recorded);
            EXPECT_EQ(sim.GetTexture(viewId), recorded);
            OceanShadowSamplingGPU sampling{};
            ASSERT_TRUE(sim.FillSampling(viewId, frame, sampling));
            EXPECT_FLOAT_EQ(sampling.Layout.CascadeOriginScale[0][0], 992.0f);
            other.BeginFrame(frame.FrameIndex());
            EXPECT_FALSE(sim.FillSampling(viewId, other, sampling));
            EXPECT_EQ(sim.GetTexture(viewId, other), recorded);
            // The same view cannot dispatch twice in one application epoch.
            declaration.Declare(other, 7u, 2048.0f, false, false);
            EXPECT_FALSE(OceanShadowProbeNode::s_Declared);
            EXPECT_FALSE(sim.FillSampling(viewId, other, sampling));
            EXPECT_EQ(sim.GetTexture(viewId, frame), pending);
            frame.AddPass("OceanShadow.Abort", PassPhase::kFinalize,
                [](RenderGraph::RGPassBuilder& pass)
                {
                    pass.PreventCulling();
                    pass.Prepare([](RenderGraph::RGPrepareContext&) { return false; });
                }, [](RenderGraph::RGContext&) {});
            const auto before = frame.SubmittedFrameCount();
            for (int retry = 0; retry < 2; ++retry)
            {
                frame.Execute();
                EXPECT_EQ(frame.SubmittedFrameCount(), before);
                EXPECT_EQ(sim.GetTexture(viewId), recorded);
                EXPECT_EQ(sim.GetTexture(viewId, frame), pending);
            }
            device->FinalizeFrame();
            device->WaitForIdle();

            ASSERT_TRUE(device->BeginFrame());
            declaration.Declare(frame, 8u, 0.0f, false);
            ASSERT_TRUE(OceanShadowProbeNode::s_Declared);
            // A late input/camera edit cannot alter this graph's UBO or output layout.
            OceanInputDrawPacket late{};
            late.Family = OceanInputFamily::Shadow;
            late.ExtentX = late.ExtentZ = 1000.0f;
            late.Value[0] = late.Value[1] = 1.0f;
            sim.SetInputs({late});
            camera.cameraPos[0] = 2048.0f;
            rs.Views().SetCameraData(cameraId, ::GameEngine::Tests::TestCameraWithEye(camera), {0.1f, 1000.0f});
            ASSERT_TRUE(sim.FillSampling(viewId, frame, sampling));
            EXPECT_FLOAT_EQ(sampling.Layout.CascadeOriginScale[0][0], -32.0f);
            frame.Execute();
            EXPECT_EQ(frame.SubmittedFrameCount(), before + 1u);
            device->FinalizeFrame();
            device->WaitForIdle();
            EXPECT_NEAR(ReadCentreVisibility(*device, sim.GetTexture(viewId)), 1.0f, 0.002f)
                << "a skipped graph frame breaks temporal blending";

            const auto retained = sim.GetTexture(viewId);
            sim.ForgetFrame(frame);
            EXPECT_EQ(sim.GetTexture(viewId), retained);
            EXPECT_FALSE(sim.FillSampling(viewId, frame, sampling));
            ASSERT_TRUE(device->BeginFrame());
            declaration.Declare(frame, 9u, 0.0f, true);
            frame.Execute();
            device->FinalizeFrame();
            device->WaitForIdle();
            EXPECT_NEAR(ReadCentreVisibility(*device, sim.GetTexture(viewId)), 0.0f, 0.002f)
                << "a retired stream's numeric successor must not reuse its identity";

            ASSERT_TRUE(device->BeginFrame());
            declaration.Declare(other, 10u, 0.0f, false);
            ASSERT_TRUE(OceanShadowProbeNode::s_Declared);
            other.Execute();
            device->FinalizeFrame();
            device->WaitForIdle();
            EXPECT_NEAR(ReadCentreVisibility(*device, sim.GetTexture(viewId)), 1.0f, 0.002f)
                << "a different stream's consecutive counter must not blend the first stream's dark history";

            // Replace this view's history after declaration. The old packet still
            // owns its arrays, but cannot publish into the replacement entry.
            ASSERT_TRUE(device->BeginFrame());
            declaration.Declare(frame, 11u, 1024.0f, true);
            const auto obsolete = sim.GetTexture(viewId, frame);
            settings.Resolution = 64u;
            sim.SetSettings(settings);
            declaration.Declare(other, 100u, 0.0f, false);
            ASSERT_TRUE(OceanShadowProbeNode::s_Declared);
            const auto replacement = sim.GetTexture(viewId, other);
            EXPECT_NE(replacement, obsolete);
            EXPECT_FALSE(sim.GetTexture(viewId).IsValid());
            const auto obsoleteSubmissions = frame.SubmittedFrameCount();
            frame.Execute();
            EXPECT_EQ(frame.SubmittedFrameCount(), obsoleteSubmissions);
            EXPECT_FALSE(sim.GetTexture(viewId).IsValid());
            EXPECT_EQ(sim.GetTexture(viewId, other), replacement);
            device->WaitForIdle();
            other.Execute();
            EXPECT_EQ(sim.GetTexture(viewId), replacement);
            device->FinalizeFrame();
            device->WaitForIdle();
            // B and C choose the same next ping-pong output. B stays alive after its abort.
            FramePools stalePools(device.get());
            RenderGraph::RGFrame stale(device.get(), &stalePools.Persistent,
                                       &stalePools.Transient, &stalePools.Ring);
            bool allowStaleSubmission = false;
            ASSERT_TRUE(device->BeginFrame());
            declaration.Declare(stale, 101u, 0.0f, true);
            ASSERT_TRUE(OceanShadowProbeNode::s_Declared);
            const auto staleOutput = sim.GetTexture(viewId, stale);
            stale.AddPass("OceanShadow.RetryGate", PassPhase::kFinalize,
                [&](RenderGraph::RGPassBuilder& pass)
                {
                    pass.PreventCulling();
                    pass.Prepare([&allowStaleSubmission](RenderGraph::RGPrepareContext&)
                    {
                        return allowStaleSubmission;
                    });
                }, [](RenderGraph::RGContext&) {});
            const auto staleSubmissions = stale.SubmittedFrameCount();
            stale.Execute();
            EXPECT_EQ(stale.SubmittedFrameCount(), staleSubmissions);
            device->FinalizeFrame();
            device->WaitForIdle();

            ASSERT_TRUE(device->BeginFrame());
            declaration.Declare(other, 102u, 0.0f, false);
            ASSERT_TRUE(OceanShadowProbeNode::s_Declared);
            EXPECT_EQ(sim.GetTexture(viewId, other), staleOutput);
            other.Execute();
            device->FinalizeFrame();
            device->WaitForIdle();
            ASSERT_EQ(sim.GetTexture(viewId), staleOutput);
            EXPECT_NEAR(ReadCentreVisibility(*device, staleOutput, settings.Resolution), 1.0f, 0.002f);
            OceanShadowSamplingGPU currentSampling{};
            ASSERT_TRUE(sim.FillSampling(viewId, other, currentSampling));

            allowStaleSubmission = true;
            ASSERT_TRUE(device->BeginFrame());
            const auto staleFailures = stale.Stats().PreparationFailures;
            stale.Execute();
            EXPECT_EQ(stale.SubmittedFrameCount(), staleSubmissions);
            EXPECT_GT(stale.Stats().PreparationFailures, staleFailures);
            EXPECT_EQ(sim.GetTexture(viewId), staleOutput);
            OceanShadowSamplingGPU afterRetry{};
            ASSERT_TRUE(sim.FillSampling(viewId, other, afterRetry));
            EXPECT_EQ(std::memcmp(&currentSampling, &afterRetry, sizeof(afterRetry)), 0);
            device->FinalizeFrame();
            device->WaitForIdle();
            EXPECT_NEAR(ReadCentreVisibility(*device, staleOutput, settings.Resolution), 1.0f, 0.002f)
                << "the old dark graph must not overwrite the newer lit output";

            // A consumed owner plan cannot execute the same step twice either.
            ASSERT_TRUE(device->BeginFrame());
            const auto currentSubmissions = other.SubmittedFrameCount();
            other.Execute();
            EXPECT_EQ(other.SubmittedFrameCount(), currentSubmissions);
            device->FinalizeFrame();
            device->WaitForIdle();
            EXPECT_NEAR(ReadCentreVisibility(*device, staleOutput, settings.Resolution), 1.0f, 0.002f);
            sim.ForgetFrame(stale);
            sim.ForgetFrame(other);
        }

        OceanShadowProbeNode::s_Sim = nullptr;
        depthEmit.Reset();
        rs.Shutdown();
    }
    device->Shutdown();
}

} // namespace

TEST(OceanShadowSimTest, DeclaresEveryFrameAcrossDeviceSlotWraps)
{
    CheckShadowHistory(GraphicsAPI::Vulkan, false);
}
TEST(OceanShadowSimTest, AbortsAndOtherStreamsCannotPublishUnrecordedHistory)
{
    CheckShadowHistory(GraphicsAPI::Vulkan, true);
}
#if defined(__APPLE__)
TEST(MetalOceanShadowSimTest, DeclaresEveryFrameAcrossDeviceSlotWraps)
{
    CheckShadowHistory(GraphicsAPI::Metal, false);
}
TEST(MetalOceanShadowSimTest, AbortsAndOtherStreamsCannotPublishUnrecordedHistory)
{
    CheckShadowHistory(GraphicsAPI::Metal, true);
}
#endif
