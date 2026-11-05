#include <gtest/gtest.h>

#include "Ocean/OceanClipSim.h"
#include "Ocean/OceanDynWaves.h"
#include "Ocean/OceanFFT.h"
#include "Ocean/OceanFoamSim.h"
#include "Ocean/OceanGPUQuery.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PassPhase.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Materials/ShaderCompileService.h"
#include "Rendering/Materials/ShaderLayoutShape.h"
#include "TestTempDir.h"

#include <cmath>
#include <cstring>
#include <memory>
#include <type_traits>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;
using namespace GameEngine::Ocean;

namespace
{
std::string SampleChecksum(const std::vector<float>& data)
{
    uint64_t hash = 1469598103934665603ull;
    const auto* bytes = reinterpret_cast<const uint8_t*>(data.data());
    for (size_t i = 0; i < data.size() * sizeof(float); ++i)
        hash = (hash ^ bytes[i]) * 1099511628211ull;
    return std::to_string(hash);
}

DescriptorBinding ComputeBinding(uint32_t slot, DescriptorType type)
{
    DescriptorBinding value{};
    value.binding = slot;
    value.type = type;
    value.count = 1;
    value.shaderStages = kShaderStageCompute;
    return value;
}

class OceanPreparedDispatchTest : public ::testing::Test
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
        m_Device = DeviceFactory::CreateDevice(desc);
        if (!m_Device || !m_Device->Initialize(desc))
            GTEST_SKIP() << "Requested Ocean test backend unavailable";
        ASSERT_EQ(m_Device->GetAPI(), RequestedAPI());
    }
    void TearDown() override
    {
        if (!m_Device)
            return;
        m_Device->WaitForIdle();
        for (const auto buffer : m_Buffers)
            if (buffer.IsValid())
                m_Device->DestroyBuffer(buffer);
        m_Device->Shutdown();
    }
    void CheckPreparedClipDispatch(GraphicsAPI expectedAPI)
    {
        ASSERT_EQ(m_Device->GetAPI(), expectedAPI);
        OceanClipSim clip;
        ASSERT_TRUE(clip.Initialize(m_Device.get()));
        clip.ConfigureCascades(m_Device.get(), 8.0f, 8u);

        TestUtils::ScopedTempDir probeCache{TestUtils::MakeUniqueTempDirectory("ocean_prepared_probe")};
        ShaderProgramCompileRequest request{};
        request.debugName = "OceanPreparedClipProbe";
        request.baseDirectory = probeCache.Path();
        request.cacheRoot = probeCache.Path() / "ShaderCache";
        ShaderStageCompileSpec stage{};
        stage.stage = "cs";
        stage.sourcePath = probeCache.Path() / "clip_probe.comp";
        stage.inlineSource = R"glsl(#version 450
layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;
layout(std430, set = 0, binding = 0) buffer Result { float values[4]; } result;
layout(set = 0, binding = 1) uniform sampler2DArray field;
void main()
{
    for (uint i = 0; i < 4; ++i)
        result.values[i] = textureLod(field, vec3(0.5, 0.5, 0.0), 0.0).r;
}
)glsl";
        request.stages.push_back(std::move(stage));
        ShaderProgramCompileResult compiled{};
        std::string error;
        ASSERT_TRUE(ShaderCompileService::CompileProgramToCache(
            request, ShaderSourceKind::SpirV, compiled, &error)) << error;
        ASSERT_TRUE(compiled.stageBytes.contains("cs"));
        DescriptorSetLayoutDesc probeLayout{};
        probeLayout.bindings = {ComputeBinding(0, DescriptorType::StorageBuffer),
                                ComputeBinding(1, DescriptorType::CombinedImageSampler)};
        ApplyMetaImageShapeToLayout(compiled.meta, probeLayout);
        ComputePipelineDesc desc{};
        desc.ComputeShader = std::make_shared<const std::vector<uint8_t>>(compiled.stageBytes.at("cs"));
        desc.DescriptorSetLayouts.push_back(m_Device->InternDescriptorSetLayout(probeLayout));
        const auto probePipeline = m_Device->GetOrCreateComputePipeline(m_Device->InternComputePipeline(desc));
        ASSERT_TRUE(probePipeline.IsValid());

        auto sample = [&]()
        {
            std::array<float, 4> values{-17.0f, -17.0f, -17.0f, -17.0f};
            const auto resultBuffer = MakeBuffer(sizeof(values), BufferUsage::Storage, values.data());
            DescriptorSetDesc set{};
            set.layout = probeLayout;
            const auto probeSet = m_Device->CreateDescriptorSet(set);
            m_Device->UpdateStorageBufferBinding(probeSet, 0, resultBuffer, 0, sizeof(values));
            m_Device->UpdateCombinedImageSamplerBinding(probeSet, 1, clip.GetClipTexture(), clip.GetClipSampler());
            auto commands = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
            commands->Begin();
            commands->SetPipeline(probePipeline);
            commands->BindDescriptorSet(0, probeSet, probePipeline);
            commands->Dispatch(1, 1, 1);
            commands->End();
            m_Device->ExecuteCommandLists({commands.get()});
            m_Device->WaitForIdle();
            if (void* data = m_Device->MapBuffer(resultBuffer))
            {
                std::memcpy(values.data(), data, sizeof(values));
                m_Device->UnmapBuffer(resultBuffer);
            }
            return values;
        };

        RenderGraph::RGResourcePool persistent(m_Device.get());
        RenderGraph::RGTransientPool transient(m_Device.get());
        RenderGraph::RGUploadRing uploads(m_Device.get(), 2, 262144);
        RenderGraph::RGFrame frame(m_Device.get(), &persistent, &transient, &uploads);
        for (uint32_t index = 0; index < 3; ++index)
        {
            SCOPED_TRACE(::testing::Message() << "frame " << index);
            ASSERT_TRUE(m_Device->BeginFrame());
            frame.BeginFrame(index);
            clip.SetDefaultClippingState(index == 0 ? 1.0f : 0.0f);
            EXPECT_TRUE(clip.BeginFrame(index == 0 ? 0.0f : 100.0f, 0.0f));
            const auto params = frame.AllocUpload<OceanClipParamsGPU>();
            ASSERT_TRUE(params.Valid());
            clip.FillParams(*params.Ptr);
            auto* prepared = frame.AllocatePassData<OceanPreparedDispatch>();
            bool recorded = false;
            const auto observer = frame.AddPass("OceanClip.PrepareWitness", PassPhase::kEarlySetup,
                [](RenderGraph::RGPassBuilder& pass) { pass.PreventCulling(); },
                [&](RenderGraph::RGContext&)
                {
                    EXPECT_TRUE(prepared->Pipeline.IsValid());
                    EXPECT_TRUE(prepared->Set.IsValid());
                    EXPECT_TRUE(clip.NeedsDispatch());
                });
            const auto target = clip.ImportRG(frame);
            const auto writer = frame.AddPass("OceanClip.PreparedWriter", PassPhase::kEarlySetup,
                [&](RenderGraph::RGPassBuilder& pass)
                {
                    pass.UsePipeline(clip.GetPipelineId());
                    pass.Write(target, RenderGraph::RGTextureWrite::Storage);
                    pass.Prepare([&, prepared, buffer = params.Buffer, offset = params.Offset](RenderGraph::RGPrepareContext& ctx)
                    {
                        *prepared = clip.PrepareDispatch(ctx.GetDevice(), buffer, offset);
                        return prepared->IsValid();
                    });
                },
                [&](RenderGraph::RGContext& ctx)
                {
                    clip.RecordDispatch(ctx.Cmd, *prepared);
                    recorded = true;
                });
            frame.AddOrderingEdge(observer, writer);
            if (index == 1)
            {
                const auto abort = frame.AddPass("OceanClip.Abort", PassPhase::kFinalize,
                    [](RenderGraph::RGPassBuilder& pass)
                    {
                        pass.PreventCulling();
                        pass.Prepare([](RenderGraph::RGPrepareContext&) { return false; });
                    }, [](RenderGraph::RGContext&) {});
                frame.AddOrderingEdge(writer, abort);
            }
            frame.MarkOutput(target, RenderGraph::RGImageLayout::ShaderReadOnly);
            const auto before = frame.SubmittedFrameCount();
            frame.Execute();
            EXPECT_TRUE(prepared->IsValid());
            EXPECT_EQ(recorded, index != 1);
            EXPECT_EQ(clip.NeedsDispatch(), index == 1);
            EXPECT_EQ(frame.Stats().PreparationFailures, index == 1 ? 1u : 0u);
            EXPECT_EQ(frame.SubmittedFrameCount(), before + (index == 1 ? 0u : 1u));
            m_Device->FinalizeFrame();
            m_Device->WaitForIdle();
            for (const float value : sample())
                EXPECT_FLOAT_EQ(value, index == 2 ? 0.0f : 1.0f);
        }
    }

    void CheckPreparedFFTDispatch(GraphicsAPI expectedAPI)
    {
        ASSERT_EQ(m_Device->GetAPI(), expectedAPI);
        OceanFFT recovered, control;
        ASSERT_TRUE(recovered.Initialize(m_Device.get()));
        ASSERT_TRUE(control.Initialize(m_Device.get()));
        OceanFFTParamsGPU settings{};
        settings.Time = 0.375f;
        for (uint32_t octave = 0; octave < 16; ++octave)
        {
            settings.SpectrumPower[octave] = 1.0e-4f;
            settings.ChopScales[octave] = 1.0f;
            settings.GravityScales[octave] = 1.0f;
        }
        recovered.SetParams(settings, true);
        control.SetParams(settings, true);

        TestUtils::ScopedTempDir cache{TestUtils::MakeUniqueTempDirectory("ocean_fft_probe")};
        ShaderProgramCompileRequest request{};
        request.debugName = "OceanPreparedFFTProbe";
        request.baseDirectory = cache.Path();
        request.cacheRoot = cache.Path() / "ShaderCache";
        ShaderStageCompileSpec stage{};
        stage.stage = "cs";
        stage.sourcePath = cache.Path() / "fft_probe.comp";
        stage.inlineSource = R"glsl(#version 450
layout(local_size_x = 4, local_size_y = 4, local_size_z = 1) in;
layout(std430, set = 0, binding = 0) buffer Result { vec4 values[]; } result;
layout(set = 0, binding = 1) uniform sampler2DArray field;
void main()
{
    uvec3 p = gl_GlobalInvocationID;
    uint index = (p.z * 4 + p.y) * 4 + p.x;
    result.values[index] = textureLod(field, vec3((vec2(p.xy) + 0.5) / 4.0, float(p.z)), 0.0);
}
)glsl";
        request.stages.push_back(std::move(stage));
        ShaderProgramCompileResult compiled{};
        std::string error;
        ASSERT_TRUE(ShaderCompileService::CompileProgramToCache(
            request, ShaderSourceKind::SpirV, compiled, &error)) << error;
        ASSERT_TRUE(compiled.stageBytes.contains("cs"));
        DescriptorSetLayoutDesc layout{};
        layout.bindings = {ComputeBinding(0, DescriptorType::StorageBuffer),
                           ComputeBinding(1, DescriptorType::CombinedImageSampler)};
        ApplyMetaImageShapeToLayout(compiled.meta, layout);
        ComputePipelineDesc desc{};
        desc.ComputeShader = std::make_shared<const std::vector<uint8_t>>(compiled.stageBytes.at("cs"));
        desc.DescriptorSetLayouts.push_back(m_Device->InternDescriptorSetLayout(layout));
        const auto probe = m_Device->GetOrCreateComputePipeline(m_Device->InternComputePipeline(desc));
        ASSERT_TRUE(probe.IsValid());
        auto sample = [&](const OceanFFT& fft)
        {
            std::vector<float> values(4u * 4u * fft.GetCascadeCount() * 4u, -17.0f);
            const auto output = MakeBuffer(values.size() * sizeof(float), BufferUsage::Storage, values.data());
            DescriptorSetDesc setDesc{};
            setDesc.layout = layout;
            const auto set = m_Device->CreateDescriptorSet(setDesc);
            m_Device->UpdateStorageBufferBinding(set, 0, output, 0, values.size() * sizeof(float));
            m_Device->UpdateCombinedImageSamplerBinding(set, 1, fft.GetDisplacementTexture(), fft.GetSampler());
            auto commands = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
            commands->Begin();
            commands->SetPipeline(probe);
            commands->BindDescriptorSet(0, set, probe);
            commands->Dispatch(1, 1, fft.GetCascadeCount());
            commands->End();
            m_Device->ExecuteCommandLists({commands.get()});
            m_Device->WaitForIdle();
            if (void* data = m_Device->MapBuffer(output))
            {
                std::memcpy(values.data(), data, values.size() * sizeof(float));
                m_Device->UnmapBuffer(output);
            }
            return values;
        };
        RenderGraph::RGResourcePool persistent(m_Device.get());
        RenderGraph::RGTransientPool transient(m_Device.get());
        RenderGraph::RGUploadRing uploads(m_Device.get(), 2, 65536);
        RenderGraph::RGFrame frame(m_Device.get(), &persistent, &transient, &uploads);
        uint32_t frameIndex = 0;
        auto dispatch = [&](OceanFFT& fft, bool abort, bool initialize)
        {
            SCOPED_TRACE(::testing::Message() << "FFT frame " << frameIndex);
            ASSERT_TRUE(m_Device->BeginFrame());
            frame.BeginFrame(frameIndex++);
            const auto params = frame.AllocUpload<OceanFFTParamsGPU>();
            ASSERT_TRUE(params.Valid());
            fft.FillParams(*params.Ptr);
            auto* prepared = frame.AllocatePassData<OceanPreparedFFTDispatch>();
            bool recorded = false;
            const auto observer = frame.AddPass("OceanFFT.PrepareWitness", PassPhase::kEarlySetup,
                [](RenderGraph::RGPassBuilder& pass) { pass.PreventCulling(); },
                [&](RenderGraph::RGContext&)
                {
                    EXPECT_TRUE(prepared->Update.Set.IsValid());
                    EXPECT_TRUE(prepared->Horizontal.Set.IsValid());
                    EXPECT_TRUE(prepared->Vertical.Set.IsValid());
                    EXPECT_EQ(prepared->Init.Set.IsValid(), initialize);
                });
            const auto output = fft.ImportDisplacementRG(frame);
            const auto writer = frame.AddPass("OceanFFT.PreparedWriter", PassPhase::kEarlySetup,
                [&](RenderGraph::RGPassBuilder& pass)
                {
                    for (const auto pipeline : fft.GetPipelineIds())
                        pass.UsePipeline(pipeline);
                    pass.Write(output, RenderGraph::RGTextureWrite::Storage);
                    pass.Prepare([&, prepared, buffer = params.Buffer, offset = params.Offset](RenderGraph::RGPrepareContext& ctx)
                    {
                        *prepared = fft.PrepareDispatch(ctx.GetDevice(), buffer, offset);
                        return prepared->IsValid();
                    });
                },
                [&](RenderGraph::RGContext& ctx)
                {
                    fft.RecordDispatch(ctx.Cmd, *prepared);
                    recorded = true;
                });
            frame.AddOrderingEdge(observer, writer);
            if (abort)
            {
                const auto failure = frame.AddPass("OceanFFT.Abort", PassPhase::kFinalize,
                    [](RenderGraph::RGPassBuilder& pass)
                    {
                        pass.PreventCulling();
                        pass.Prepare([](RenderGraph::RGPrepareContext&) { return false; });
                    }, [](RenderGraph::RGContext&) {});
                frame.AddOrderingEdge(writer, failure);
            }
            frame.MarkOutput(output, RenderGraph::RGImageLayout::ShaderReadOnly);
            const auto before = frame.SubmittedFrameCount();
            frame.Execute();
            EXPECT_TRUE(prepared->IsValid());
            EXPECT_EQ(prepared->InitializeSpectrum, initialize);
            EXPECT_EQ(recorded, !abort);
            EXPECT_EQ(frame.Stats().PreparationFailures, abort ? 1u : 0u);
            EXPECT_EQ(frame.SubmittedFrameCount(), before + (abort ? 0u : 1u));
            m_Device->FinalizeFrame();
            m_Device->WaitForIdle();
        };
        ASSERT_NO_FATAL_FAILURE(dispatch(recovered, true, true));
        ASSERT_NO_FATAL_FAILURE(dispatch(recovered, false, true));
        ASSERT_NO_FATAL_FAILURE(dispatch(control, false, true));
        const auto first = sample(recovered);
        EXPECT_EQ(first, sample(control));
        bool nonzero = false;
        for (size_t i = 0; i < first.size(); ++i)
        {
            EXPECT_TRUE(std::isfinite(first[i]));
            if (i % 4u != 3u && std::abs(first[i]) > 1.0e-7f)
                nonzero = true;
        }
        EXPECT_TRUE(nonzero) << "the GPU comparison must contain actual waves";
        settings.Time = 0.75f;
        recovered.SetParams(settings, false);
        control.SetParams(settings, false);
        ASSERT_NO_FATAL_FAILURE(dispatch(recovered, false, false));
        ASSERT_NO_FATAL_FAILURE(dispatch(control, false, false));
        const auto later = sample(recovered);
        EXPECT_EQ(later, sample(control));
        EXPECT_NE(later, first) << "the update must use the new time slice";
        RecordProperty("fftFirstSamples", SampleChecksum(first));
        RecordProperty("fftLaterSamples", SampleChecksum(later));
    }

    template<class Simulation>
    void CheckPreparedTemporalDispatch(GraphicsAPI expectedAPI)
    {
        ASSERT_EQ(m_Device->GetAPI(), expectedAPI);
        constexpr bool kFoam = std::is_same_v<Simulation, OceanFoamSim>;
        using Params = std::conditional_t<kFoam, OceanFoamParamsGPU, OceanDynWavesParamsGPU>;
        Simulation recovered, control;
        OceanCascadeArray displacement;
        ASSERT_TRUE(displacement.Initialize(m_Device.get(), 16, 4, TextureFormat::R16G16B16A16_FLOAT,
                                             64.0f, "TemporalDummyDisplacement"));
        for (auto* sim : {&recovered, &control})
        {
            ASSERT_TRUE(sim->Initialize(m_Device.get()));
            sim->ConfigureCascades(m_Device.get(), 64.0f, 16u);
            if constexpr (kFoam)
                sim->ConfigureSimulation(20.0f, 4u);
            else
                sim->ConfigureSimulation(20.0f, 4u, 0.1f, 0.6f, 9.81f, 0.75f, 0.4f, 2.0f, 0u, 3u);
            sim->InvalidateHistory();
        }
        const auto texture = [](const Simulation& sim)
        {
            if constexpr (kFoam) return sim.GetFoamTexture();
            else return sim.GetTexture();
        };
        const auto layoutOf = [](const Simulation& sim) -> const OceanCascadeLayoutGPU&
        {
            if constexpr (kFoam) return sim.GetFoamLayout();
            else return sim.GetLayout();
        };
        const auto sampler = [](const Simulation& sim)
        {
            if constexpr (kFoam) return sim.GetFoamSampler();
            else return sim.GetSampler();
        };
        const auto inputs = [&](Simulation& sim, float dt, bool impulse)
        {
            if constexpr (kFoam)
            {
                sim.SetFrameInputs(displacement.GetTexture(), sampler(sim), 0u, 0.8f, 0.55f, 1.0f, dt,
                                    0.0f, 15.0f, 10.0f, 1.0f, 10.0f, 0.0f, 0.0f);
                OceanFoamInputGPU source{};
                source.OriginExtent[2] = source.OriginExtent[3] = 24.0f;
                source.AmountFeatherBlend[0] = 0.5f;
                sim.SetAuthoredSources(&source, 1u);
            }
            else
            {
                OceanWaveImpulseGPU source{};
                source.CenterRadiusAmp[2] = 12.0f;
                source.CenterRadiusAmp[3] = 0.5f;
                sim.SetFrameInputs(impulse ? &source : nullptr, impulse ? 1u : 0u, 4.0f, 0.2f, dt);
            }
        };

        TestUtils::ScopedTempDir cache{TestUtils::MakeUniqueTempDirectory("ocean_temporal_probe")};
        ShaderProgramCompileRequest request{};
        request.debugName = "OceanPreparedTemporalProbe";
        request.baseDirectory = cache.Path();
        request.cacheRoot = cache.Path() / "ShaderCache";
        ShaderStageCompileSpec stage{};
        stage.stage = "cs";
        stage.sourcePath = cache.Path() / "probe.comp";
        stage.inlineSource = R"glsl(#version 450
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout(std430, set = 0, binding = 0) buffer Result { vec4 values[]; } result;
layout(set = 0, binding = 1) uniform sampler2DArray field;
void main()
{
    uvec3 p = gl_GlobalInvocationID;
    result.values[(p.z * 16 + p.y) * 16 + p.x] =
        texelFetch(field, ivec3(p), 0);
}
)glsl";
        request.stages.push_back(std::move(stage));
        ShaderProgramCompileResult compiled{};
        std::string error;
        ASSERT_TRUE(ShaderCompileService::CompileProgramToCache(
            request, ShaderSourceKind::SpirV, compiled, &error)) << error;
        DescriptorSetLayoutDesc probeLayout{};
        probeLayout.bindings = {ComputeBinding(0, DescriptorType::StorageBuffer),
                                ComputeBinding(1, DescriptorType::CombinedImageSampler)};
        ApplyMetaImageShapeToLayout(compiled.meta, probeLayout);
        ComputePipelineDesc desc{};
        desc.ComputeShader = std::make_shared<const std::vector<uint8_t>>(compiled.stageBytes.at("cs"));
        desc.DescriptorSetLayouts.push_back(m_Device->InternDescriptorSetLayout(probeLayout));
        const auto probe = m_Device->GetOrCreateComputePipeline(m_Device->InternComputePipeline(desc));
        ASSERT_TRUE(probe.IsValid());
        const auto sample = [&](const Simulation& sim)
        {
            std::vector<float> values(16u * 16u * 4u * 4u, -17.0f);
            const auto output = MakeBuffer(values.size() * sizeof(float), BufferUsage::Storage, values.data());
            DescriptorSetDesc setDesc{};
            setDesc.layout = probeLayout;
            const auto set = m_Device->CreateDescriptorSet(setDesc);
            m_Device->UpdateStorageBufferBinding(set, 0, output, 0, values.size() * sizeof(float));
            m_Device->UpdateCombinedImageSamplerBinding(set, 1, texture(sim), sampler(sim));
            auto commands = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
            commands->Begin();
            commands->SetPipeline(probe);
            commands->BindDescriptorSet(0, set, probe);
            commands->Dispatch(2, 2, 4);
            commands->End();
            m_Device->ExecuteCommandLists({commands.get()});
            m_Device->WaitForIdle();
            void* data = m_Device->MapBuffer(output);
            EXPECT_NE(data, nullptr);
            if (data)
            {
                std::memcpy(values.data(), data, values.size() * sizeof(float));
                m_Device->UnmapBuffer(output);
            }
            return values;
        };
        RenderGraph::RGResourcePool persistent(m_Device.get());
        RenderGraph::RGTransientPool transient(m_Device.get());
        RenderGraph::RGUploadRing uploads(m_Device.get(), 2, 65536);
        RenderGraph::RGFrame frame(m_Device.get(), &persistent, &transient, &uploads);
        RenderGraph::RGUploadRing otherUploads(m_Device.get(), 2, 65536);
        RenderGraph::RGFrame other(m_Device.get(), &persistent, &transient, &otherUploads);
        uint32 frameIndex = 1u;
        const auto dispatch = [&](Simulation& sim, uint32 count, float cameraX, bool abort, bool reset,
                                  RenderGraph::RGFrame* retainedGraph = nullptr,
                                  bool* allowSubmission = nullptr)
        {
            auto& graph = retainedGraph ? *retainedGraph : frame;
            SCOPED_TRACE(::testing::Message() << "simulation frame " << frameIndex);
            const auto committedTexture = texture(sim);
            const auto committedLayout = layoutOf(sim);
            ASSERT_EQ(sim.GetPendingSubstepCount(), count);
            ASSERT_TRUE(m_Device->BeginFrame());
            graph.BeginFrame(frameIndex++);
            const auto& plan = sim.PlanSubsteps(graph, cameraX, 0.0f);
            ASSERT_EQ(plan.Steps.size(), count);
            ASSERT_GT(count, 0u);
            EXPECT_EQ(std::memcmp(&plan.Steps.front().PrevCascade, &committedLayout,
                                   sizeof(committedLayout)), 0);
            EXPECT_EQ(&sim.PlanSubsteps(graph, cameraX + 1000.0f, 0.0f), &plan);
            EXPECT_EQ(texture(sim), committedTexture);
            EXPECT_EQ(std::memcmp(&layoutOf(sim), &committedLayout, sizeof(committedLayout)), 0);
            other.BeginFrame(frameIndex);
            if constexpr (kFoam)
            {
                EXPECT_EQ(sim.GetFoamTexture(other), committedTexture);
                EXPECT_EQ(std::memcmp(&sim.GetFoamLayout(other), &committedLayout, sizeof(committedLayout)), 0);
                EXPECT_EQ(std::memcmp(&sim.GetFoamLayout(graph), &plan.Steps.back().Cascade, sizeof(committedLayout)), 0);
            }
            else
            {
                EXPECT_EQ(sim.GetTexture(other), committedTexture);
                EXPECT_EQ(std::memcmp(&sim.GetLayout(other), &committedLayout, sizeof(committedLayout)), 0);
                EXPECT_EQ(std::memcmp(&sim.GetLayout(graph), &plan.Steps.back().Cascade, sizeof(committedLayout)), 0);
            }
            auto* packets = graph.AllocatePassData<std::vector<OceanPreparedSimulationDispatch*>>();
            const auto observer = graph.AddPass("OceanTemporal.PrepareWitness", PassPhase::kEarlySetup,
                [](RenderGraph::RGPassBuilder& pass) { pass.PreventCulling(); },
                [&sim, packets, count](RenderGraph::RGContext&)
                {
                    for (auto* packet : *packets)
                    {
                        EXPECT_TRUE(packet->Commands.Pipeline.IsValid());
                        EXPECT_TRUE(packet->Commands.Set.IsValid());
                    }
                    EXPECT_EQ(sim.GetPendingSubstepCount(), count);
                });
            RenderGraph::RGPass last = observer;
            for (uint32 index = 0; index < count; ++index)
            {
                const auto step = plan.Steps[index];
                if (index > 0)
                {
                    EXPECT_NE(step.WriteIndex, plan.Steps[index - 1u].WriteIndex);
                    EXPECT_EQ(std::memcmp(&step.PrevCascade, &plan.Steps[index - 1u].Cascade,
                                           sizeof(step.Cascade)), 0);
                }
                const auto params = graph.AllocUpload<Params>();
                ASSERT_TRUE(params.Valid());
                sim.FillParams(*params.Ptr, step, index);
                EXPECT_FLOAT_EQ(params.Ptr->DeltaTime, 0.05f);
                EXPECT_EQ(params.Ptr->ResetHistory, reset && index == 0u ? 1u : 0u);
                if constexpr (!kFoam)
                    EXPECT_EQ(params.Ptr->ImpulseCount, index == 0u ? 1u : 0u);
                auto* packet = graph.AllocatePassData<OceanPreparedSimulationDispatch>();
                packets->push_back(packet);
                const auto output = sim.ImportWriteRG(graph, step.WriteIndex);
                const auto previous = sim.ImportPrevRG(graph, step.WriteIndex);
                const auto pass = graph.AddPass("OceanTemporal.Step", PassPhase::kEarlySetup,
                    [&](RenderGraph::RGPassBuilder& pass)
                    {
                        pass.UsePipeline(sim.GetPipelineId());
                        pass.Read(previous, RenderGraph::RGTextureRead::SampledCompute);
                        pass.Write(output, RenderGraph::RGTextureWrite::Storage);
                        pass.Prepare([&sim, packet, ownerPlan = &plan, index,
                                      buffer = params.Buffer, offset = params.Offset](RenderGraph::RGPrepareContext& ctx)
                        {
                            *packet = sim.PrepareDispatch(ctx.GetDevice(), buffer, offset, *ownerPlan, index);
                            return packet->IsValid();
                        });
                    },
                    [&sim, packet](RenderGraph::RGContext& ctx) { sim.RecordDispatch(ctx.Cmd, *packet); });
                graph.AddOrderingEdge(last, pass);
                last = pass;
                graph.MarkOutput(output, RenderGraph::RGImageLayout::ShaderReadOnly);
            }
            if (abort)
            {
                const auto failure = graph.AddPass("OceanTemporal.Abort", PassPhase::kFinalize,
                    [allowSubmission](RenderGraph::RGPassBuilder& pass)
                    {
                        pass.PreventCulling();
                        pass.Prepare([allowSubmission](RenderGraph::RGPrepareContext&)
                        {
                            return allowSubmission && *allowSubmission;
                        });
                    }, [](RenderGraph::RGContext&) {});
                graph.AddOrderingEdge(last, failure);
            }
            for (uint32 attempt = 0; attempt < (abort ? 2u : 1u); ++attempt)
            {
                const auto submitted = graph.SubmittedFrameCount();
                const auto preparationFailures = graph.Stats().PreparationFailures;
                graph.Execute();
                EXPECT_EQ(graph.SubmittedFrameCount(), submitted + (abort ? 0u : 1u));
                EXPECT_EQ(graph.Stats().PreparationFailures, preparationFailures + (abort ? 1u : 0u));
                for (auto* packet : *packets)
                    EXPECT_TRUE(packet->IsValid());
                EXPECT_EQ(sim.GetPendingSubstepCount(), abort ? count : 0u);
                if (abort)
                {
                    EXPECT_EQ(texture(sim), committedTexture);
                    EXPECT_EQ(std::memcmp(&layoutOf(sim), &committedLayout, sizeof(committedLayout)), 0);
                }
            }
            m_Device->FinalizeFrame();
            m_Device->WaitForIdle();
            if (!abort)
                EXPECT_EQ(std::memcmp(&layoutOf(sim), &plan.Steps.back().Cascade, sizeof(committedLayout)), 0);
        };
        inputs(recovered, 0.1f, true);
        inputs(control, 0.1f, true);
        ASSERT_NO_FATAL_FAILURE(dispatch(recovered, 2u, 0.0f, false, true));
        ASSERT_NO_FATAL_FAILURE(dispatch(control, 2u, 0.0f, false, true));
        const auto first = sample(recovered);
        EXPECT_EQ(first, sample(control));
        RecordProperty("temporalFirstSamples", SampleChecksum(first));
        bool nonzero = false;
        for (size_t i = 0; i < first.size(); ++i)
        {
            EXPECT_TRUE(std::isfinite(first[i]));
            if (i % 4u == 0u && std::abs(first[i]) > 1.0e-7f)
                nonzero = true;
        }
        ASSERT_TRUE(nonzero) << "the GPU comparison must contain simulated data";
        recovered.InvalidateHistory();
        control.InvalidateHistory();
        inputs(recovered, 0.15f, true);
        ASSERT_NO_FATAL_FAILURE(dispatch(recovered, 3u, 8.0f, true, true));
        EXPECT_EQ(first, sample(recovered));
        inputs(recovered, 0.0f, false);
        inputs(control, 0.15f, true);
        ASSERT_NO_FATAL_FAILURE(dispatch(recovered, 3u, 8.0f, false, true));
        ASSERT_NO_FATAL_FAILURE(dispatch(control, 3u, 8.0f, false, true));
        const auto recoveredValues = sample(recovered);
        EXPECT_EQ(recoveredValues, sample(control));
        EXPECT_NE(recoveredValues, first);
        RecordProperty("temporalResetRecoverySamples", SampleChecksum(recoveredValues));
        inputs(recovered, 0.15f, true);
        ASSERT_NO_FATAL_FAILURE(dispatch(recovered, 3u, 16.0f, true, false));
        EXPECT_EQ(recoveredValues, sample(recovered));
        inputs(recovered, 0.0f, false);
        inputs(control, 0.15f, true);
        ASSERT_NO_FATAL_FAILURE(dispatch(recovered, 3u, 16.0f, false, false));
        ASSERT_NO_FATAL_FAILURE(dispatch(control, 3u, 16.0f, false, false));
        const auto historyRecovery = sample(recovered);
        EXPECT_EQ(historyRecovery, sample(control));
        RecordProperty("temporalHistoryRecoverySamples", SampleChecksum(historyRecovery));
        // Keep the aborted graph and its uploads alive while a newer graph records.
        RenderGraph::RGUploadRing staleUploads(m_Device.get(), 2, 65536);
        RenderGraph::RGFrame stale(m_Device.get(), &persistent, &transient, &staleUploads);
        bool allowStaleSubmission = false;
        inputs(recovered, 0.15f, true);
        ASSERT_NO_FATAL_FAILURE(dispatch(recovered, 3u, 128.0f, true, false,
                                         &stale, &allowStaleSubmission));
        inputs(recovered, 0.0f, false);
        inputs(control, 0.15f, true);
        ASSERT_NO_FATAL_FAILURE(dispatch(recovered, 3u, 0.0f, false, false));
        ASSERT_NO_FATAL_FAILURE(dispatch(control, 3u, 0.0f, false, false));
        const auto beforeStaleRetry = sample(recovered);
        ASSERT_EQ(beforeStaleRetry, sample(control));
        const auto recordedTexture = texture(recovered);
        const auto recordedLayout = layoutOf(recovered);
        // These new inputs must remain pending when the older graph is refused.
        inputs(recovered, 0.1f, true);
        inputs(control, 0.1f, true);
        ASSERT_EQ(recovered.GetPendingSubstepCount(), 2u);
        allowStaleSubmission = true;
        ASSERT_TRUE(m_Device->BeginFrame());
        const auto staleSubmissions = stale.SubmittedFrameCount();
        const auto staleFailures = stale.Stats().PreparationFailures;
        stale.Execute();
        EXPECT_EQ(stale.SubmittedFrameCount(), staleSubmissions);
        EXPECT_GT(stale.Stats().PreparationFailures, staleFailures);
        EXPECT_EQ(texture(recovered), recordedTexture);
        EXPECT_EQ(std::memcmp(&layoutOf(recovered), &recordedLayout, sizeof(recordedLayout)), 0);
        EXPECT_EQ(recovered.GetPendingSubstepCount(), 2u);
        m_Device->FinalizeFrame();
        m_Device->WaitForIdle();
        EXPECT_EQ(sample(recovered), beforeStaleRetry)
            << "refusing publication after dispatch cannot protect the recorded texture";
        ASSERT_NO_FATAL_FAILURE(dispatch(recovered, 2u, 0.0f, false, false));
        ASSERT_NO_FATAL_FAILURE(dispatch(control, 2u, 0.0f, false, false));
        EXPECT_EQ(sample(recovered), sample(control))
            << "the refused graph must not consume newer time or impulses";
        recovered.ForgetFrame(stale);
        recovered.ForgetFrame(frame);
        other.BeginFrame(frameIndex++);
        EXPECT_TRUE(recovered.PlanSubsteps(other, 1000.0f, 0.0f).Steps.empty());
        if constexpr (kFoam)
            EXPECT_EQ(recovered.GetFoamTexture(other), texture(recovered));
        else
            EXPECT_EQ(recovered.GetTexture(other), texture(recovered));
        recovered.ForgetFrame(other);
    }

    void CheckCompletedQueryBatches(GraphicsAPI expectedAPI)
    {
        ASSERT_EQ(m_Device->GetAPI(), expectedAPI);
        OceanGPUQuery query;
        OceanFFT fft;
        ASSERT_TRUE(query.Initialize(m_Device.get()));
        ASSERT_TRUE(fft.Initialize(m_Device.get()));
        query.SetFrameInputs(fft.GetDisplacementTexture(), fft.GetSampler(), {}, {});
        RenderGraph::RGResourcePool persistent(m_Device.get());
        RenderGraph::RGTransientPool transient(m_Device.get());
        RenderGraph::RGUploadRing uploads(m_Device.get(), 2, 65536);
        RenderGraph::RGFrame frame(m_Device.get(), &persistent, &transient, &uploads);
        OceanGPUQueryToken tokens[2]{};
        for (uint32 index = 0; index < 2; ++index)
        {
            SCOPED_TRACE(index);
            ASSERT_TRUE(m_Device->BeginFrame());
            frame.BeginFrame(index);
            const OceanSurfaceQueryPoint point{12.0f + index, -8.0f};
            tokens[index] = query.Enqueue(&point, 1u);
            OceanParamsGPU ocean{};
            ocean.SeaLevel = 17.0f + index;
            ocean.FFTCascadeCount = 0u;
            const auto token = query.PrepareNext(ocean, {}, false);
            ASSERT_EQ(token, tokens[index]);
            const auto params = frame.AllocUpload<OceanGPUQueryParamsGPU>();
            ASSERT_TRUE(params.Valid());
            query.FillParams(*params.Ptr);
            auto* prepared = frame.AllocatePassData<OceanPreparedQueryDispatch>();
            frame.AddPass("OceanQuery.CompletedBatch", PassPhase::kEarlySetup,
                [&](RenderGraph::RGPassBuilder& pass)
                {
                    pass.UsePipeline(query.GetPipelineId());
                    pass.PreventCulling();
                    pass.Prepare([&, prepared, token, buffer = params.Buffer, offset = params.Offset,
                                  values = params.Ptr](RenderGraph::RGPrepareContext& ctx)
                    {
                        return query.PrepareDispatch(ctx.GetDevice(), frame, buffer, offset,
                                                     *values, token, *prepared);
                    });
                },
                [&, prepared](RenderGraph::RGContext& ctx)
                {
                    EXPECT_TRUE(prepared->Readback.Commands.Pipeline.IsValid());
                    EXPECT_TRUE(prepared->Readback.Commands.Set.IsValid());
                    query.RecordDispatch(ctx.Cmd, *prepared);
                });
            const auto submitted = frame.SubmittedFrameCount();
            frame.Execute();
            ASSERT_EQ(frame.SubmittedFrameCount(), submitted + 1u);
            query.OnFrameSubmitted(frame, frame.SubmissionToken());
            m_Device->FinalizeFrame();
            m_Device->WaitForIdle();
        }
        query.PollCompleted(m_Device.get());
        for (uint32 index = 0; index < 2; ++index)
        {
            SCOPED_TRACE(index);
            std::vector<OceanSurfaceSample> samples;
            const bool completed = query.TryTake(tokens[index], samples);
            EXPECT_TRUE(completed);
            if (!completed)
                continue;
            ASSERT_EQ(samples.size(), 1u);
            EXPECT_FLOAT_EQ(samples[0].PositionWS[0], 12.0f + index);
            EXPECT_FLOAT_EQ(samples[0].Height, 17.0f + index);
            EXPECT_FLOAT_EQ(samples[0].PositionWS[2], -8.0f);
            EXPECT_FLOAT_EQ(samples[0].NormalWS[1], 1.0f);
            EXPECT_EQ(samples[0].Source, OceanQuerySource::GPUQuery);
        }
    }

    BufferHandle MakeBuffer(size_t size, BufferUsage usage, const void* data)
    {
        BufferDesc desc{};
        desc.size = size;
        desc.usage = static_cast<uint32_t>(usage);
        desc.memoryUsage = BufferMemoryUsage::Upload;
        auto buffer = m_Device->CreateBuffer(desc);
        m_Buffers.push_back(buffer);
        if (buffer.IsValid())
        {
            void* mapped = m_Device->MapBuffer(buffer);
            EXPECT_NE(mapped, nullptr);
            if (mapped)
                std::memcpy(mapped, data, size);
            m_Device->UnmapBuffer(buffer);
        }
        return buffer;
    }

    std::unique_ptr<IDevice> m_Device;
    std::vector<BufferHandle> m_Buffers;
    TestUtils::ScopedTempDir m_Cache{TestUtils::MakeUniqueTempDirectory("ocean_prepared_dispatch")};
};
} // namespace

TEST_F(OceanPreparedDispatchTest, EveryCompletedQueryTokenSurvivesUntilPolled)
{
    CheckCompletedQueryBatches(GraphicsAPI::Vulkan);
}

TEST_F(OceanPreparedDispatchTest, ClipPreparesBeforeRecordingAndRetriesAfterAbort)
{
    CheckPreparedClipDispatch(GraphicsAPI::Vulkan);
}

TEST_F(OceanPreparedDispatchTest, DynamicStepsPrepareBeforeRecordingAndSurviveAbort)
{
    CheckPreparedTemporalDispatch<OceanDynWaves>(GraphicsAPI::Vulkan);
}


TEST_F(OceanPreparedDispatchTest, FoamStepsPrepareBeforeRecordingAndSurviveAbort)
{
    CheckPreparedTemporalDispatch<OceanFoamSim>(GraphicsAPI::Vulkan);
}


TEST_F(OceanPreparedDispatchTest, FFTFirstUseAbortRecoversTheControlAndKeepsItsSpectrum)
{
    CheckPreparedFFTDispatch(GraphicsAPI::Vulkan);
}

#if defined(__APPLE__)
class MetalOceanPreparedDispatchTest : public OceanPreparedDispatchTest
{
  protected:
    GraphicsAPI RequestedAPI() const override { return GraphicsAPI::Metal; }
};

TEST_F(MetalOceanPreparedDispatchTest, ClipPreparesBeforeRecordingAndRetriesAfterAbort)
{
    CheckPreparedClipDispatch(GraphicsAPI::Metal);
}
TEST_F(MetalOceanPreparedDispatchTest, FFTFirstUseAbortRecoversTheControlAndKeepsItsSpectrum)
{
    CheckPreparedFFTDispatch(GraphicsAPI::Metal);
}
TEST_F(MetalOceanPreparedDispatchTest, DynamicStepsPrepareBeforeRecordingAndSurviveAbort)
{
    CheckPreparedTemporalDispatch<OceanDynWaves>(GraphicsAPI::Metal);
}

TEST_F(MetalOceanPreparedDispatchTest, FoamStepsPrepareBeforeRecordingAndSurviveAbort)
{
    CheckPreparedTemporalDispatch<OceanFoamSim>(GraphicsAPI::Metal);
}
TEST_F(MetalOceanPreparedDispatchTest, EveryCompletedQueryTokenSurvivesUntilPolled)
{
    CheckCompletedQueryBatches(GraphicsAPI::Metal);
}
#endif
