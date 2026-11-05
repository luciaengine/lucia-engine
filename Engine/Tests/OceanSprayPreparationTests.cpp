#include <gtest/gtest.h>

#include "Engine/Rendering/Pipeline/PipelineResourceNames.h"
#include "Engine/Rendering/Pipeline/RenderPipeline.h"
#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"
#include "Engine/Rendering/RenderServices.h"
#include "Mathematics/Matrix4x4.h"
#include "Ocean/OceanRenderFeature.h"
#include "Rendering/Core/BarrierMapping.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Materials/ShaderCompileService.h"
#include "Rendering/Materials/ShaderLayoutShape.h"
#include "TestTempDir.h"
#include "Types/Color.h"

#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;
using namespace GameEngine::Engine::Renderer;
using namespace GameEngine::Engine::Renderer::Pipeline;
using namespace GameEngine::Ocean;

namespace
{
constexpr uint32 kExtent = 64;
constexpr uint32 kCapacity = 256;
struct alignas(16) SprayParticleSample
{
    float PositionAge[4]{}, VelocityLife[4]{}, Look[4]{}, Radiance[4]{};
};
static_assert(sizeof(SprayParticleSample) == 64);
struct SpraySnapshot
{
    SprayParticleSample Particle;
    uint32 Args[4]{};
    uint32 Visible = 0;
};
constexpr size_t kSnapshotBytes = sizeof(SprayParticleSample) + 5u * sizeof(uint32);

struct SprayPools
{
    RenderGraph::RGResourcePool Persistent;
    RenderGraph::RGTransientPool Transient;
    RenderGraph::RGUploadRing Uploads;
    explicit SprayPools(IDevice* device) : Persistent(device), Transient(device), Uploads(device, 3, 262144) {}
};

struct DeviceFrame
{
    IDevice& Device;
    bool Active;
    explicit DeviceFrame(IDevice& device) : Device(device), Active(device.BeginFrame()) {}
    ~DeviceFrame()
    {
        if (Active)
        {
            Device.FinalizeFrame();
            Device.WaitForIdle();
        }
    }
};

struct SprayProbeNode final : IRenderPipelineNode
{
    static bool Declared;
    const char* GetTypeName() const override { return "OceanSprayProbe"; }
    bool Initialize(std::string, std::string, std::string*) override { return true; }
    void DeclareForView(ViewDeclare& d) override
    {
        d.PublishTexture(Names::Res::SceneColor, d.ViewColor);
        auto& ocean = d.Services.EnsureFeature<OceanRenderFeature>();
        Declared = ocean.GetSprayGPU().DeclareForView(d, ocean);
    }
};
bool SprayProbeNode::Declared = false;

TextureDesc SprayTarget(TextureFormat format, TextureUsage usage)
{
    TextureDesc desc{};
    desc.width = desc.height = kExtent;
    desc.depth = desc.arrayLayers = desc.mipLevels = desc.sampleCount = 1;
    desc.format = static_cast<uint32>(format);
    desc.usage = static_cast<uint32>(usage);
    return desc;
}

DescriptorBinding SprayBinding(uint32 binding, DescriptorType type)
{
    DescriptorBinding result{};
    result.binding = binding;
    result.type = type;
    result.count = 1;
    result.shaderStages = kShaderStageCompute;
    return result;
}

class OceanSprayPreparationTest : public ::testing::Test
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
            GTEST_SKIP() << "Requested ocean spray backend unavailable";
        ASSERT_EQ(m_Device->GetAPI(), RequestedAPI());
        m_Services = std::make_unique<RenderServices>();
        ASSERT_TRUE(m_Services->Initialize(m_Device.get()));
        m_Camera = m_Services->Views().AllocateCamera("OceanSprayCamera");
        CameraData camera{};
        const auto view = Mathematics::Matrix4x4::LookAt(
            Mathematics::Vector3(0, 1, 0), Mathematics::Vector3(0, 1, 1), Mathematics::Vector3(0, 1, 0));
        const auto projection = Mathematics::Matrix4x4::PerspectiveReverseZ(1.0471976f, 1.0f, 0.1f, 100.0f);
        const auto viewProjection = projection * view;
        std::memcpy(camera.view, view.Data(), sizeof(camera.view));
        std::memcpy(camera.proj, projection.Data(), sizeof(camera.proj));
        std::memcpy(camera.viewProj, viewProjection.Data(), sizeof(camera.viewProj));
        camera.cameraPos[1] = 1.0f;
        m_Services->Views().SetCameraData(m_Camera, camera);
        m_View = m_Services->Views().AllocateView("OceanSprayView", m_Camera);
        m_Services->Views().SetViewRenderLayerMask(m_View, 1);
        ViewClearConfig clear{};
        m_Services->Views().SetViewTargets(m_View, 0, 0, 0, clear);
        m_Ocean = &m_Services->EnsureFeature<OceanRenderFeature>();
        ASSERT_TRUE(m_Ocean->Initialize(m_Device.get()));
        ASSERT_TRUE(m_Ocean->IsFoamReady());
        m_Ocean->GetFoamSim().ConfigureCascades(m_Device.get(), 8.0f, 8);
        m_Settings.Spray = true;
        m_Settings.SprayRenderLayerMask = 1;
        m_Settings.SpraySpawnRate = 1.0f;
        m_Settings.SprayWindThreshold = 0;
        m_Settings.SprayWindVelocityScale = 0;
        m_Settings.SprayGeometryIntersections = false;
        SetCapacity(kCapacity);
        ASSERT_TRUE(m_Registry.Register("OceanSprayProbe", [] { return std::make_unique<SprayProbeNode>(); }, true));
        RenderPipelineBlueprint blueprint;
        blueprint.pipelineName = "OceanSprayPreparation";
        RenderPipelineBlueprint::Pass pass{};
        pass.id = "Spray";
        pass.type = "OceanSprayProbe";
        pass.enabled = pass.perView = true;
        pass.passJson = R"({"id":"Spray","type":"OceanSprayProbe"})";
        blueprint.passes.push_back(pass);
        m_Pipeline = std::make_unique<RenderPipelineInstance>(*m_Services, m_Registry);
        m_Pipeline->SetBlueprint(blueprint);
        InitializeProbe();
        ASSERT_TRUE(m_ProbePipeline.IsValid());
        InitializeFoam();
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
        for (const auto buffer : m_Buffers)
            m_Device->DestroyBuffer(buffer);
        m_Device->Shutdown();
    }

    PipelineHandle Compile(const char* name, const char* source, DescriptorSetLayoutDesc& layout)
    {
        ShaderProgramCompileRequest request{};
        request.debugName = name;
        request.baseDirectory = m_Cache.Path();
        request.cacheRoot = m_Cache.Path() / "ShaderCache";
        ShaderStageCompileSpec stage{};
        stage.stage = "cs";
        stage.sourcePath = m_Cache.Path() / (std::string(name) + ".comp");
        stage.inlineSource = source;
        request.stages.push_back(std::move(stage));
        ShaderProgramCompileResult result{};
        std::string error;
        if (!ShaderCompileService::CompileProgramToCache(request, ShaderSourceKind::SpirV, result, &error))
        {
            ADD_FAILURE() << error;
            return {};
        }
        ApplyMetaImageShapeToLayout(result.meta, layout);
        ComputePipelineDesc desc{};
        desc.ComputeShader = std::make_shared<const std::vector<uint8_t>>(result.stageBytes.at("cs"));
        desc.DescriptorSetLayouts.push_back(m_Device->InternDescriptorSetLayout(layout));
        return m_Device->GetOrCreateComputePipeline(m_Device->InternComputePipeline(desc));
    }
    BufferHandle Buffer(size_t bytes, BufferUsage usage, BufferMemoryUsage memory = BufferMemoryUsage::Auto)
    {
        BufferDesc desc{};
        desc.size = bytes;
        desc.usage = static_cast<uint32>(usage);
        desc.memoryUsage = memory;
        const auto buffer = m_Device->CreateBuffer(desc);
        m_Buffers.push_back(buffer);
        return buffer;
    }
    void InitializeProbe()
    {
        m_ProbeLayout.bindings = {SprayBinding(0, DescriptorType::StorageBuffer),
            SprayBinding(1, DescriptorType::StorageBuffer), SprayBinding(2, DescriptorType::StorageBuffer),
            SprayBinding(3, DescriptorType::StorageBuffer)};
        m_ProbePipeline = Compile("OceanSprayReadbackProbe", R"glsl(#version 450
layout(local_size_x = 32) in;
layout(std430, set=0, binding=0) readonly buffer Particle { uint particle[]; };
layout(std430, set=0, binding=1) readonly buffer Arguments { uint args[]; };
layout(std430, set=0, binding=2) readonly buffer Visible { uint visible[]; };
layout(std430, set=0, binding=3) writeonly buffer Result { uint result[]; };
void main()
{
    uint i = gl_GlobalInvocationID.x;
    if (i < 16) result[i] = particle[i];
    else if (i < 20) result[i] = args[i - 16];
    else if (i == 20) result[i] = args[1] > 0 ? visible[0] : 0xffffffffu;
}
)glsl", m_ProbeLayout);
        m_Scratch = Buffer(kSnapshotBytes, BufferUsage::Storage | BufferUsage::TransferSrc);
        m_Readback = Buffer(kSnapshotBytes, BufferUsage::TransferDst, BufferMemoryUsage::Readback);
        m_ColorReadback = Buffer(kExtent * kExtent * 4u, BufferUsage::TransferDst, BufferMemoryUsage::Readback);
    }
    void InitializeFoam()
    {
        DescriptorSetLayoutDesc layout{};
        layout.bindings = {SprayBinding(0, DescriptorType::StorageImage)};
        const auto pipeline = Compile("OceanSprayFoamInitialize", R"glsl(#version 450
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout(rgba16f, set=0, binding=0) uniform writeonly image2DArray field;
void main() { imageStore(field, ivec3(gl_GlobalInvocationID), vec4(0)); }
)glsl", layout);
        ASSERT_TRUE(pipeline.IsValid());
        DescriptorSetDesc desc{};
        desc.layout = layout;
        const auto set = m_Device->CreateDescriptorSet(desc);
        const auto field = m_Ocean->GetFoamSim().GetFoamTexture();
        m_Device->UpdateStorageImageBinding(set, 0, field);
        auto commands = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        commands->Begin();
        commands->Barrier(ResourceBarrier::CreateTextureBarrier(field, ResourceState::ShaderResource,
                                                                 ResourceState::UnorderedAccess));
        commands->SetPipeline(pipeline);
        commands->BindDescriptorSet(0, set, pipeline);
        commands->Dispatch(1, 1, m_Ocean->GetFoamSim().GetLodCount());
        commands->Barrier(ResourceBarrier::CreateTextureBarrier(field, ResourceState::UnorderedAccess,
                                                                 ResourceState::ShaderResource));
        commands->End();
        m_Device->ExecuteCommandLists({commands.get()});
        m_Device->WaitForIdle();
        m_Device->DestroyDescriptorSet(set);
    }
    void SetCapacity(uint32 capacity)
    {
        m_Settings.SprayMaxParticles = capacity;
        m_Ocean->GetSprayGPU().SetSettings(m_Settings, 10.0f, 0);
    }
    bool Declare(RenderGraph::RGFrame& frame, uint64 index, float time, bool advanceEpoch = true)
    {
        frame.BeginFrame(index);
        if (advanceEpoch)
            m_Services->BeginWorldDrawFrame();
        m_Services->BuildWorldBatchKeys();
        OceanParamsGPU params{};
        params.Time = time;
        params.SeaLevel = 0;
        params.WaveMode = 0;
        params.GerstnerWaveCount = 0;
        params.FFTCascadeCount = 0;
        m_Ocean->SetParams(params);
        const auto color = frame.ImportPersistentTexture("OceanSprayProbe.Color",
            SprayTarget(TextureFormat::RGBA8_UNORM, TextureUsage::RenderTarget | TextureUsage::TransferSrc));
        const auto depth = frame.ImportPersistentTexture("OceanSprayProbe.Depth",
            SprayTarget(TextureFormat::D32_FLOAT, TextureUsage::DepthStencil | TextureUsage::ShaderResource));
        frame.AddPass("OceanSprayProbe.Clear", PassPhase::kEarlySetup,
            [color, depth, background = m_Background](RenderGraph::RGPassBuilder& p) {
                RenderGraph::RGAttachmentOps colorOps{}, depthOps{};
                colorOps.Load = depthOps.Load = RenderGraph::RGLoadOp::Clear;
                colorOps.Clear.Color[0] = background.r;
                colorOps.Clear.Color[1] = background.g;
                colorOps.Clear.Color[2] = background.b;
                colorOps.Clear.Color[3] = background.a;
                p.AttachColor(0, color, colorOps);
                p.AttachDepth(depth, depthOps);
            }, [](RenderGraph::RGContext&) {});
        SprayProbeNode::Declared = false;
        const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{m_View, color, depth, {}}};
        const std::vector<ViewDesc> views(m_Services->Views().GetViews().begin(), m_Services->Views().GetViews().end());
        m_Pipeline->Declare(frame, targets, views);
        frame.MarkOutput(color);
        return SprayProbeNode::Declared;
    }
    BufferHandle NamedBuffer(const RenderGraph::RGFrame& frame, const char* suffix) const
    {
        const auto name = "Ocean.Spray.View" + std::to_string(m_View) + suffix;
        const auto buffer = frame.FindBuffer(name.c_str());
        return buffer.IsValid() ? frame.PhysicalBuffer(buffer) : BufferHandle{};
    }
    void Seed(const RenderGraph::RGFrame& frame, float x = 0)
    {
        SprayParticleSample particle{};
        particle.PositionAge[0] = x;
        particle.PositionAge[1] = 1;
        particle.PositionAge[2] = 2;
        particle.PositionAge[3] = 0.2f;
        particle.VelocityLife[3] = 2;
        particle.Look[0] = particle.Look[1] = 0.4f;
        particle.Look[2] = 0.7f;
        particle.Look[3] = 0.5f;
        const auto state = NamedBuffer(frame, ".State");
        ASSERT_TRUE(state.IsValid());
        m_Device->UpdateBuffer(state, 0, sizeof(particle), &particle);
        m_Device->WaitForIdle();
    }
    SpraySnapshot Observe(const RenderGraph::RGFrame& frame)
    {
        SpraySnapshot result{};
        DescriptorSetDesc desc{};
        desc.layout = m_ProbeLayout;
        const auto set = m_Device->CreateDescriptorSet(desc);
        m_Device->UpdateStorageBufferBinding(set, 0, NamedBuffer(frame, ".State"), 0, sizeof(SprayParticleSample));
        m_Device->UpdateStorageBufferBinding(set, 1, NamedBuffer(frame, ".Args"), 0, 16);
        m_Device->UpdateStorageBufferBinding(set, 2, NamedBuffer(frame, ".Visible"), 0, 4);
        m_Device->UpdateStorageBufferBinding(set, 3, m_Scratch, 0, kSnapshotBytes);
        auto commands = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        commands->Begin();
        commands->Barrier(ResourceBarrier::CreateMemoryBarrier(
            static_cast<uint64>(PipelineStageMask::ComputeShader | PipelineStageMask::Transfer),
            static_cast<uint64>(PipelineStageMask::ComputeShader),
            static_cast<uint64>(ResourceAccessMask::ShaderWrite | ResourceAccessMask::TransferWrite),
            static_cast<uint64>(ResourceAccessMask::ShaderRead)));
        commands->SetPipeline(m_ProbePipeline);
        commands->BindDescriptorSet(0, set, m_ProbePipeline);
        commands->Dispatch(1, 1, 1);
        commands->Barrier(ResourceBarrier::CreateBufferBarrier(m_Scratch, ResourceState::UnorderedAccess,
                                                                ResourceState::CopySource));
        commands->CopyBuffer(m_Scratch, m_Readback, kSnapshotBytes);
        commands->Barrier(ResourceBarrier::CreateBufferBarrier(m_Scratch, ResourceState::CopySource,
                                                                ResourceState::UnorderedAccess));
        commands->End();
        m_Device->ExecuteCommandLists({commands.get()});
        m_Device->WaitForIdle();
        if (const auto* mapped = static_cast<const uint8*>(m_Device->MapBuffer(m_Readback)))
        {
            std::memcpy(&result.Particle, mapped, sizeof(result.Particle));
            std::memcpy(result.Args, mapped + sizeof(result.Particle), sizeof(result.Args));
            std::memcpy(&result.Visible, mapped + sizeof(result.Particle) + sizeof(result.Args), sizeof(result.Visible));
            m_Device->UnmapBuffer(m_Readback);
        }
        else
            ADD_FAILURE() << "spray readback was not mapped";
        m_Device->DestroyDescriptorSet(set);
        return result;
    }
    std::array<uint8, 4> CenterPixel(const RenderGraph::RGFrame& frame)
    {
        std::array<uint8, 4> pixel{};
        const auto color = frame.PhysicalTexture(frame.FindTexture("OceanSprayProbe.Color"));
        auto commands = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        commands->Begin();
        commands->Barrier(ResourceBarrier::CreateTextureBarrier(color, ResourceState::RenderTarget,
                                                                 ResourceState::CopySource));
        commands->CopyTextureToBuffer(color, m_ColorReadback, kExtent, kExtent);
        commands->Barrier(ResourceBarrier::CreateTextureBarrier(color, ResourceState::CopySource,
                                                                 ResourceState::RenderTarget));
        commands->End();
        m_Device->ExecuteCommandLists({commands.get()});
        m_Device->WaitForIdle();
        if (const auto* mapped = static_cast<const uint8*>(m_Device->MapBuffer(m_ColorReadback)))
        {
            std::memcpy(pixel.data(), mapped + ((kExtent / 2u) * kExtent + kExtent / 2u) * 4u, 4u);
            m_Device->UnmapBuffer(m_ColorReadback);
        }
        else
            ADD_FAILURE() << "spray pixel readback was not mapped";
        return pixel;
    }
    void AddAbort(RenderGraph::RGFrame& frame, bool& abort)
    {
        const auto abortPass = frame.AddPass("OceanSprayProbe.Abort", PassPhase::kFinalize,
            [&abort](RenderGraph::RGPassBuilder& p) {
                p.PreventCulling();
                p.Prepare([&abort](RenderGraph::RGPrepareContext&) { return !abort; });
            }, [](RenderGraph::RGContext&) {});
        for (RenderGraph::RGPassId id = 0; id < frame.Graph().PassCount(); ++id)
            if (std::strcmp(frame.Graph().PassName(id), "OceanSprayDraw") == 0)
                frame.AddOrderingEdge(RenderGraph::RGPass{id}, abortPass);
    }
    void Warm(RenderGraph::RGFrame& frame, uint64 index, float time)
    {
        DeviceFrame deviceFrame(*m_Device);
        ASSERT_TRUE(deviceFrame.Active);
        ASSERT_TRUE(Declare(frame, index, time));
        const auto before = frame.SubmittedFrameCount();
        frame.Execute();
        ASSERT_EQ(frame.SubmittedFrameCount(), before + 1);
    }
    void CheckPreparationAndRetry()
    {
        SprayPools pools(m_Device.get());
        RenderGraph::RGFrame frame(m_Device.get(), &pools.Persistent, &pools.Transient, &pools.Uploads);
        Warm(frame, 100, 1.0f);
        ASSERT_FALSE(HasFatalFailure());
        const auto emptyPixel = CenterPixel(frame);
        Seed(frame);
        bool abort = true, beforeRecord = false, afterRecord = false;
        uint32 firstAllocations = 0;
        {
            DeviceFrame deviceFrame(*m_Device);
            ASSERT_TRUE(deviceFrame.Active);
            ASSERT_TRUE(Declare(frame, 107, 1.0f));
            const auto count = frame.Graph().PassCount();
            RenderGraph::RGPass simulate{}, draw{};
            for (RenderGraph::RGPassId id = 0; id < count; ++id)
            {
                if (std::strcmp(frame.Graph().PassName(id), "OceanSpraySimulate") == 0) simulate.Id = id;
                if (std::strcmp(frame.Graph().PassName(id), "OceanSprayDraw") == 0) draw.Id = id;
            }
            ASSERT_TRUE(simulate.IsValid());
            ASSERT_TRUE(draw.IsValid());
            const auto before = frame.AddPass("OceanSprayProbe.Before", PassPhase::kPostProcess,
                [](RenderGraph::RGPassBuilder& p) { p.PreventCulling(); },
                [this, &beforeRecord, &firstAllocations](RenderGraph::RGContext&) {
                    beforeRecord = true;
                    firstAllocations = m_Device->GetDescriptorAllocatorStats().TransientAllocated;
                });
            const auto after = frame.AddPass("OceanSprayProbe.After", PassPhase::kPostProcess,
                [](RenderGraph::RGPassBuilder& p) { p.PreventCulling(); },
                [this, &afterRecord, &firstAllocations](RenderGraph::RGContext&) {
                    afterRecord = true;
                    if (RequestedAPI() == GraphicsAPI::Vulkan)
                        EXPECT_EQ(m_Device->GetDescriptorAllocatorStats().TransientAllocated, firstAllocations);
                });
            frame.AddOrderingEdge(before, simulate);
            frame.AddOrderingEdge(draw, after);
            AddAbort(frame, abort);
            const auto initial = m_Device->GetDescriptorAllocatorStats().TransientAllocated;
            for (uint32 retry = 0; retry < 2; ++retry)
            {
                const auto submissions = frame.SubmittedFrameCount();
                frame.Execute();
                EXPECT_EQ(frame.SubmittedFrameCount(), submissions);
                EXPECT_FALSE(beforeRecord);
                EXPECT_FALSE(afterRecord);
            }
            if (RequestedAPI() == GraphicsAPI::Vulkan)
                EXPECT_GE(m_Device->GetDescriptorAllocatorStats().TransientAllocated, initial + 8u);
            abort = false;
            const auto allocatedBeforeRetry = m_Device->GetDescriptorAllocatorStats().TransientAllocated;
            frame.Execute();
            EXPECT_TRUE(beforeRecord);
            EXPECT_TRUE(afterRecord);
            if (RequestedAPI() == GraphicsAPI::Vulkan)
                EXPECT_GE(firstAllocations, allocatedBeforeRetry + 4u);
        }
        const auto result = Observe(frame);
        EXPECT_FLOAT_EQ(result.Particle.PositionAge[3], 0.2f);
        EXPECT_EQ(result.Args[0], 6u);
        EXPECT_EQ(result.Args[1], 1u);
        EXPECT_EQ(result.Args[2], 0u);
        EXPECT_EQ(result.Args[3], 0u);
        EXPECT_EQ(result.Visible, 0u);
        const auto drawnPixel = CenterPixel(frame);
        EXPECT_NE(drawnPixel, emptyPixel) << "the real indirect draw must blend the seeded particle";
        EXPECT_EQ(drawnPixel[3], 255u) << "premultiplied spray over an opaque target remains opaque";
        m_Background = ColorLinear(0.25f, 0.5f, 0.75f, 1.0f);
        Warm(frame, 114, 1.0f);
        ASSERT_FALSE(HasFatalFailure());
        const auto overColor = CenterPixel(frame);
        // The unchanged particle is drawn over two backgrounds. Their difference
        // measures transmission, independently of the shader's exponential alpha.
        const float transmission = (float(overColor[2]) - float(drawnPixel[2])) / (255.0f * 0.75f);
        const float opacity = 1.0f - transmission;
        EXPECT_GT(opacity, 0.01f);
        EXPECT_LT(opacity, 0.9f);
        EXPECT_NEAR(float(drawnPixel[0]) / 255.0f, result.Particle.Radiance[0] * opacity, 0.015f);
        EXPECT_NEAR(float(drawnPixel[1]) / 255.0f, result.Particle.Radiance[1] * opacity, 0.015f);
        EXPECT_NEAR(float(overColor[0] - drawnPixel[0]) / 255.0f, 0.25f * transmission, 0.01f);
        EXPECT_EQ(overColor[3], 255u);
        m_Ocean->OnFrameStreamRetiredRG(frame);
    }
    void CheckAbortAndStaleOwner()
    {
        SprayPools pools(m_Device.get());
        RenderGraph::RGUploadRing staleUploads(m_Device.get(), 3, 262144);
        RenderGraph::RGFrame current(m_Device.get(), &pools.Persistent, &pools.Transient, &pools.Uploads);
        RenderGraph::RGFrame stale(m_Device.get(), &pools.Persistent, &pools.Transient, &staleUploads);
        Warm(current, 100, 1.0f);
        ASSERT_FALSE(HasFatalFailure());
        Seed(current, 2.0f);
        // Stream counters are independent. A retained replay's pool maintenance
        // must not precede the counter of the shared pool's final import.
        bool abort = true;
        m_Ocean->GetSprayGPU().RebaseOrigin(1, 0, 0);
        {
            DeviceFrame deviceFrame(*m_Device);
            ASSERT_TRUE(deviceFrame.Active);
            ASSERT_TRUE(Declare(stale, 114, 1.01f));
            AddAbort(stale, abort);
            stale.Execute();
            EXPECT_EQ(stale.SubmittedFrameCount(), 0u);
        }
        m_Ocean->GetSprayGPU().RebaseOrigin(1, 0, 0);
        Warm(current, 107, 1.02f);
        ASSERT_FALSE(HasFatalFailure());
        const auto recovered = Observe(current);
        EXPECT_NEAR(recovered.Particle.PositionAge[3], 0.22f, 1e-5f);
        EXPECT_NEAR(recovered.Particle.PositionAge[0], 0.0f, 0.001f)
            << "both origin shifts since the last recorded simulation must apply";
        EXPECT_EQ(recovered.Args[1], 1u);
        abort = false;
        {
            DeviceFrame deviceFrame(*m_Device);
            ASSERT_TRUE(deviceFrame.Active);
            const auto submissions = stale.SubmittedFrameCount();
            stale.Execute();
            EXPECT_EQ(stale.SubmittedFrameCount(), submissions);
            EXPECT_GT(stale.Stats().PreparationFailures, 0u);
        }
        const auto afterStale = Observe(current);
        EXPECT_EQ(std::memcmp(&afterStale.Particle, &recovered.Particle, sizeof(recovered.Particle)), 0);
        EXPECT_EQ(std::memcmp(afterStale.Args, recovered.Args, sizeof(recovered.Args)), 0);
        {
            DeviceFrame deviceFrame(*m_Device);
            ASSERT_TRUE(deviceFrame.Active);
            const auto submissions = current.SubmittedFrameCount();
            current.Execute();
            EXPECT_EQ(current.SubmittedFrameCount(), submissions);
        }
        const auto afterConsumed = Observe(current);
        EXPECT_EQ(std::memcmp(&afterConsumed.Particle, &recovered.Particle, sizeof(recovered.Particle)), 0);
        m_Ocean->OnFrameStreamRetiredRG(stale);
        m_Ocean->OnFrameStreamRetiredRG(current);
    }
    void CheckPoolAndCapacityIdentity()
    {
        SprayPools a(m_Device.get()), b(m_Device.get());
        RenderGraph::RGFrame first(m_Device.get(), &a.Persistent, &a.Transient, &a.Uploads);
        RenderGraph::RGFrame second(m_Device.get(), &b.Persistent, &b.Transient, &b.Uploads);
        Warm(first, 100, 1.0f);
        ASSERT_FALSE(HasFatalFailure());
        const auto firstState = NamedBuffer(first, ".State");
        Seed(first);
        {
            DeviceFrame deviceFrame(*m_Device);
            ASSERT_TRUE(deviceFrame.Active);
            EXPECT_FALSE(Declare(second, 100, 1.0f, false))
                << "a matching numeric counter in another stream must not claim the owner's simulation";
            EXPECT_FALSE(NamedBuffer(second, ".State").IsValid());
        }
        Warm(second, 107, 1.01f);
        ASSERT_FALSE(HasFatalFailure());
        EXPECT_NE(NamedBuffer(second, ".State"), firstState);
        Seed(second);
        Warm(first, 114, 1.02f);
        ASSERT_FALSE(HasFatalFailure());
        EXPECT_EQ(NamedBuffer(first, ".State"), firstState);
        EXPECT_FLOAT_EQ(Observe(first).Particle.VelocityLife[3], 0.0f)
            << "returning to initialized foreign-pool storage must reset stale particles";
        Seed(first);
        SetCapacity(kCapacity * 2u);
        Warm(first, 121, 1.03f);
        ASSERT_FALSE(HasFatalFailure());
        EXPECT_NE(NamedBuffer(first, ".State"), firstState);
        EXPECT_FLOAT_EQ(Observe(first).Particle.VelocityLife[3], 0.0f);
        m_Ocean->OnFrameStreamRetiredRG(first);
        m_Ocean->OnFrameStreamRetiredRG(second);
    }

    TestUtils::ScopedTempDir m_Cache{TestUtils::MakeUniqueTempDirectory("ocean_spray_preparation")};
    std::unique_ptr<IDevice> m_Device;
    std::unique_ptr<RenderServices> m_Services;
    RenderPipelineNodeRegistry m_Registry;
    std::unique_ptr<RenderPipelineInstance> m_Pipeline;
    OceanRenderFeature* m_Ocean = nullptr;
    CameraId m_Camera{};
    ViewId m_View{};
    Components::OceanSurface m_Settings{};
    ColorLinear m_Background{0, 0, 0, 1};
    DescriptorSetLayoutDesc m_ProbeLayout;
    PipelineHandle m_ProbePipeline;
    BufferHandle m_Scratch{}, m_Readback{}, m_ColorReadback{};
    std::vector<BufferHandle> m_Buffers;
};

TEST_F(OceanSprayPreparationTest, BindingsPrepareBeforeRecordingAndRetryAfterAbort)
{
    CheckPreparationAndRetry();
}
TEST_F(OceanSprayPreparationTest, AbortedPlansCannotConsumeTimeOrOverwriteRecordedParticles)
{
    CheckAbortAndStaleOwner();
}
TEST_F(OceanSprayPreparationTest, DifferentPoolsAndCapacityDoNotReuseParticleHistory)
{
    CheckPoolAndCapacityIdentity();
}
#if defined(__APPLE__)
class MetalOceanSprayPreparationTest : public OceanSprayPreparationTest
{
  protected:
    GraphicsAPI RequestedAPI() const override { return GraphicsAPI::Metal; }
};
TEST_F(MetalOceanSprayPreparationTest, BindingsPrepareBeforeRecordingAndRetryAfterAbort)
{
    CheckPreparationAndRetry();
}
TEST_F(MetalOceanSprayPreparationTest, AbortedPlansCannotConsumeTimeOrOverwriteRecordedParticles)
{
    CheckAbortAndStaleOwner();
}
TEST_F(MetalOceanSprayPreparationTest, DifferentPoolsAndCapacityDoNotReuseParticleHistory)
{
    CheckPoolAndCapacityIdentity();
}
#endif
} // namespace
