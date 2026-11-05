#include "Ocean/OceanClipSim.h"
#include "Ocean/OceanDynWaves.h"
#include "Ocean/OceanFFT.h"
#include "Ocean/OceanFoamSim.h"
#include "Ocean/OceanGPUQuery.h"
#include "Rendering/Core/BarrierMapping.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PassPhase.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Materials/ShaderCompileService.h"
#include "Rendering/Materials/ShaderLayoutShape.h"
#include "TestTempDir.h"
#include "Types/Fnv1a.h"

#include <cmath>
#include <cstring>
#include <memory>
#include <type_traits>
#include <vector>

#include <gtest/gtest.h>

using namespace GameEngine;
using namespace GameEngine::Rendering;
using namespace GameEngine::Ocean;

namespace
{
std::string SampleChecksum(const std::vector<float>& data)
{
    return std::to_string(Hashing::Fnv1a64(data.data(), data.size() * sizeof(float)));
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
                [prepared, &clip](RenderGraph::RGContext&)
                {
                    EXPECT_TRUE(prepared->Pipeline.IsValid());
                    EXPECT_TRUE(prepared->Set.IsValid());
                    EXPECT_TRUE(clip.NeedsDispatch());
                });
            const auto target = clip.ImportRG(frame);
            const auto writer = frame.AddPass("OceanClip.PreparedWriter", PassPhase::kEarlySetup,
                [&clip, prepared, target, params](RenderGraph::RGPassBuilder& pass)
                {
                    pass.UsePipeline(clip.GetPipelineId());
                    pass.Write(target, RenderGraph::RGTextureWrite::Storage);
                    pass.Prepare([&clip, prepared, buffer = params.Buffer, offset = params.Offset](RenderGraph::RGPrepareContext& context)
                    {
                        *prepared = clip.PrepareDispatch(context.GetDevice(), buffer, offset);
                        return prepared->IsValid();
                    });
                },
                [&clip, prepared, &recorded](RenderGraph::RGContext& context)
                {
                    clip.RecordDispatch(context.Cmd, *prepared);
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
            for (const float value : SampleField(probePipeline, probeLayout, clip.GetClipTexture(),
                                                  clip.GetClipSampler(), 4u, 1u, 1u, 1u))
                EXPECT_FLOAT_EQ(value, index == 2 ? 0.0f : 1.0f);
        }
    }

    void ExecuteFFTDispatch(OceanFFT& fft, RenderGraph::RGFrame& frame, uint32& frameIndex,
                            bool abort, bool initialize)
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
            [prepared, initialize](RenderGraph::RGContext&)
            {
                EXPECT_TRUE(prepared->Update.Set.IsValid());
                EXPECT_TRUE(prepared->Horizontal.Set.IsValid());
                EXPECT_TRUE(prepared->Vertical.Set.IsValid());
                EXPECT_EQ(prepared->Init.Set.IsValid(), initialize);
            });
        const auto output = fft.ImportDisplacementRG(frame);
        const auto writer = frame.AddPass("OceanFFT.PreparedWriter", PassPhase::kEarlySetup,
            [&fft, prepared, output, params](RenderGraph::RGPassBuilder& pass)
            {
                for (const auto pipeline : fft.GetPipelineIds())
                    pass.UsePipeline(pipeline);
                pass.Write(output, RenderGraph::RGTextureWrite::Storage);
                pass.Prepare([&fft, prepared, buffer = params.Buffer, offset = params.Offset](RenderGraph::RGPrepareContext& context)
                {
                    *prepared = fft.PrepareDispatch(context.GetDevice(), buffer, offset);
                    return prepared->IsValid();
                });
            },
            [&fft, prepared, &recorded](RenderGraph::RGContext& context)
            {
                fft.RecordDispatch(context.Cmd, *prepared);
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
        const auto sample = [this, probe, &layout](const OceanFFT& fft)
        {
            return SampleField(probe, layout, fft.GetDisplacementTexture(), fft.GetSampler(),
                               4u * 4u * fft.GetCascadeCount() * 4u, 1u, 1u, fft.GetCascadeCount());
        };
        RenderGraph::RGResourcePool persistent(m_Device.get());
        RenderGraph::RGTransientPool transient(m_Device.get());
        RenderGraph::RGUploadRing uploads(m_Device.get(), 2, 65536);
        RenderGraph::RGFrame frame(m_Device.get(), &persistent, &transient, &uploads);
        uint32_t frameIndex = 0;
        ASSERT_NO_FATAL_FAILURE(ExecuteFFTDispatch(recovered, frame, frameIndex, true, true));
        ASSERT_NO_FATAL_FAILURE(ExecuteFFTDispatch(recovered, frame, frameIndex, false, true));
        ASSERT_NO_FATAL_FAILURE(ExecuteFFTDispatch(control, frame, frameIndex, false, true));
        const auto first = sample(recovered);
        EXPECT_EQ(first, sample(control));
        bool nonzero = false;
        for (size_t sampleIndex = 0; sampleIndex < first.size(); ++sampleIndex)
        {
            EXPECT_TRUE(std::isfinite(first[sampleIndex]));
            if (sampleIndex % 4u != 3u && std::abs(first[sampleIndex]) > 1.0e-7f)
                nonzero = true;
        }
        EXPECT_TRUE(nonzero) << "the GPU comparison must contain actual waves";
        settings.Time = 0.75f;
        recovered.SetParams(settings, false);
        control.SetParams(settings, false);
        ASSERT_NO_FATAL_FAILURE(ExecuteFFTDispatch(recovered, frame, frameIndex, false, false));
        ASSERT_NO_FATAL_FAILURE(ExecuteFFTDispatch(control, frame, frameIndex, false, false));
        const auto later = sample(recovered);
        EXPECT_EQ(later, sample(control));
        EXPECT_NE(later, first) << "the update must use the new time slice";
        RecordProperty("fftFirstSamples", SampleChecksum(first));
        RecordProperty("fftLaterSamples", SampleChecksum(later));
    }

    template<class Simulation>
    static TextureHandle SimulationTexture(const Simulation& simulation)
    {
        if constexpr (std::is_same_v<Simulation, OceanFoamSim>)
            return simulation.GetFoamTexture();
        else
            return simulation.GetTexture();
    }

    template<class Simulation>
    static const OceanCascadeLayoutGPU& SimulationLayout(const Simulation& simulation)
    {
        if constexpr (std::is_same_v<Simulation, OceanFoamSim>)
            return simulation.GetFoamLayout();
        else
            return simulation.GetLayout();
    }

    template<class Simulation>
    static SamplerHandle SimulationSampler(const Simulation& simulation)
    {
        if constexpr (std::is_same_v<Simulation, OceanFoamSim>)
            return simulation.GetFoamSampler();
        else
            return simulation.GetSampler();
    }

    template<class Simulation>
    static void SetTemporalInputs(Simulation& simulation, const OceanCascadeArray& displacement,
                                  float deltaTime, bool hasImpulse)
    {
        if constexpr (std::is_same_v<Simulation, OceanFoamSim>)
        {
            simulation.SetFrameInputs(displacement.GetTexture(), SimulationSampler(simulation),
                0u, 0.8f, 0.55f, 1.0f, deltaTime, 0.0f, 15.0f, 10.0f, 1.0f, 10.0f, 0.0f, 0.0f);
            OceanFoamInputGPU source{};
            source.OriginExtent[2] = source.OriginExtent[3] = 24.0f;
            source.AmountFeatherBlend[0] = 0.5f;
            simulation.SetAuthoredSources(&source, 1u);
        }
        else
        {
            OceanWaveImpulseGPU source{};
            source.CenterRadiusAmp[2] = 12.0f;
            source.CenterRadiusAmp[3] = 0.5f;
            simulation.SetFrameInputs(hasImpulse ? &source : nullptr, hasImpulse ? 1u : 0u,
                                      4.0f, 0.2f, deltaTime);
        }
    }

    template<class Simulation>
    void ExecuteTemporalDispatch(Simulation& simulation, RenderGraph::RGFrame& graph,
                                 RenderGraph::RGFrame& other, uint32& frameIndex,
                                 uint32 count, float cameraX, bool abort, bool reset,
                                 bool* allowSubmission = nullptr)
    {
        constexpr bool kFoam = std::is_same_v<Simulation, OceanFoamSim>;
        using Params = std::conditional_t<kFoam, OceanFoamParamsGPU, OceanDynWavesParamsGPU>;
        SCOPED_TRACE(::testing::Message() << "simulation frame " << frameIndex);
        const auto committedTexture = SimulationTexture(simulation);
        const auto committedLayout = SimulationLayout(simulation);
        ASSERT_EQ(simulation.GetPendingSubstepCount(), count);
        ASSERT_TRUE(m_Device->BeginFrame());
        graph.BeginFrame(frameIndex++);
        const auto& plan = simulation.PlanSubsteps(graph, cameraX, 0.0f);
        ASSERT_EQ(plan.Steps.size(), count);
        ASSERT_GT(count, 0u);
        EXPECT_EQ(std::memcmp(&plan.Steps.front().PrevCascade, &committedLayout,
                               sizeof(committedLayout)), 0);
        EXPECT_EQ(&simulation.PlanSubsteps(graph, cameraX + 1000.0f, 0.0f), &plan);
        EXPECT_EQ(SimulationTexture(simulation), committedTexture);
        EXPECT_EQ(std::memcmp(&SimulationLayout(simulation), &committedLayout, sizeof(committedLayout)), 0);
        other.BeginFrame(frameIndex);
        if constexpr (kFoam)
        {
            EXPECT_EQ(simulation.GetFoamTexture(other), committedTexture);
            EXPECT_EQ(std::memcmp(&simulation.GetFoamLayout(other), &committedLayout, sizeof(committedLayout)), 0);
            EXPECT_EQ(std::memcmp(&simulation.GetFoamLayout(graph), &plan.Steps.back().Cascade, sizeof(committedLayout)), 0);
        }
        else
        {
            EXPECT_EQ(simulation.GetTexture(other), committedTexture);
            EXPECT_EQ(std::memcmp(&simulation.GetLayout(other), &committedLayout, sizeof(committedLayout)), 0);
            EXPECT_EQ(std::memcmp(&simulation.GetLayout(graph), &plan.Steps.back().Cascade, sizeof(committedLayout)), 0);
        }
        auto* packets = graph.AllocatePassData<std::vector<OceanPreparedSimulationDispatch*>>();
        const auto observer = graph.AddPass("OceanTemporal.PrepareWitness", PassPhase::kEarlySetup,
            [](RenderGraph::RGPassBuilder& pass) { pass.PreventCulling(); },
            [&simulation, packets, count](RenderGraph::RGContext&)
            {
                for (auto* packet : *packets)
                {
                    EXPECT_TRUE(packet->Commands.Pipeline.IsValid());
                    EXPECT_TRUE(packet->Commands.Set.IsValid());
                }
                EXPECT_EQ(simulation.GetPendingSubstepCount(), count);
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
            simulation.FillParams(*params.Ptr, step, index);
            EXPECT_FLOAT_EQ(params.Ptr->DeltaTime, 0.05f);
            EXPECT_EQ(params.Ptr->ResetHistory, reset && index == 0u ? 1u : 0u);
            if constexpr (!kFoam)
                EXPECT_EQ(params.Ptr->ImpulseCount, index == 0u ? 1u : 0u);
            auto* packet = graph.AllocatePassData<OceanPreparedSimulationDispatch>();
            packets->push_back(packet);
            const auto output = simulation.ImportWriteRG(graph, step.WriteIndex);
            const auto previous = simulation.ImportPrevRG(graph, step.WriteIndex);
            const auto pass = graph.AddPass("OceanTemporal.Step", PassPhase::kEarlySetup,
                [&simulation, previous, output, packet, &plan, index, params](RenderGraph::RGPassBuilder& pass)
                {
                    pass.UsePipeline(simulation.GetPipelineId());
                    pass.Read(previous, RenderGraph::RGTextureRead::SampledCompute);
                    pass.Write(output, RenderGraph::RGTextureWrite::Storage);
                    pass.Prepare([&simulation, packet, ownerPlan = &plan, index,
                                  buffer = params.Buffer, offset = params.Offset](RenderGraph::RGPrepareContext& context)
                    {
                        *packet = simulation.PrepareDispatch(context.GetDevice(), buffer, offset, *ownerPlan, index);
                        return packet->IsValid();
                    });
                },
                [&simulation, packet](RenderGraph::RGContext& context) { simulation.RecordDispatch(context.Cmd, *packet); });
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
            EXPECT_EQ(simulation.GetPendingSubstepCount(), abort ? count : 0u);
            if (abort)
            {
                EXPECT_EQ(SimulationTexture(simulation), committedTexture);
                EXPECT_EQ(std::memcmp(&SimulationLayout(simulation), &committedLayout, sizeof(committedLayout)), 0);
            }
        }
        m_Device->FinalizeFrame();
        m_Device->WaitForIdle();
        if (!abort)
            EXPECT_EQ(std::memcmp(&SimulationLayout(simulation), &plan.Steps.back().Cascade, sizeof(committedLayout)), 0);
    }

    template<class Simulation>
    void CheckPreparedTemporalDispatch(GraphicsAPI expectedAPI)
    {
        ASSERT_EQ(m_Device->GetAPI(), expectedAPI);
        constexpr bool kFoam = std::is_same_v<Simulation, OceanFoamSim>;
        Simulation recovered, control;
        OceanCascadeArray displacement;
        ASSERT_TRUE(displacement.Initialize(m_Device.get(), 16, 4, TextureFormat::R16G16B16A16_FLOAT,
                                             64.0f, "TemporalDummyDisplacement"));
        for (auto* simulation : {&recovered, &control})
        {
            ASSERT_TRUE(simulation->Initialize(m_Device.get()));
            simulation->ConfigureCascades(m_Device.get(), 64.0f, 16u);
            if constexpr (kFoam)
                simulation->ConfigureSimulation(20.0f, 4u);
            else
                simulation->ConfigureSimulation(20.0f, 4u, 0.1f, 0.6f, 9.81f, 0.75f, 0.4f, 2.0f, 0u, 3u);
            simulation->InvalidateHistory();
        }

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
        const auto sample = [this, probe, &probeLayout](const Simulation& simulation)
        {
            return SampleField(probe, probeLayout, SimulationTexture(simulation), SimulationSampler(simulation),
                               16u * 16u * 4u * 4u, 2u, 2u, 4u);
        };
        RenderGraph::RGResourcePool persistent(m_Device.get());
        RenderGraph::RGTransientPool transient(m_Device.get());
        RenderGraph::RGUploadRing uploads(m_Device.get(), 2, 65536);
        RenderGraph::RGFrame frame(m_Device.get(), &persistent, &transient, &uploads);
        RenderGraph::RGUploadRing otherUploads(m_Device.get(), 2, 65536);
        RenderGraph::RGFrame other(m_Device.get(), &persistent, &transient, &otherUploads);
        uint32 frameIndex = 1u;
        SetTemporalInputs(recovered, displacement, 0.1f, true);
        SetTemporalInputs(control, displacement, 0.1f, true);
        ASSERT_NO_FATAL_FAILURE(ExecuteTemporalDispatch(recovered, frame, other, frameIndex, 2u, 0.0f, false, true));
        ASSERT_NO_FATAL_FAILURE(ExecuteTemporalDispatch(control, frame, other, frameIndex, 2u, 0.0f, false, true));
        const auto first = sample(recovered);
        EXPECT_EQ(first, sample(control));
        RecordProperty("temporalFirstSamples", SampleChecksum(first));
        bool nonzero = false;
        for (size_t sampleIndex = 0; sampleIndex < first.size(); ++sampleIndex)
        {
            EXPECT_TRUE(std::isfinite(first[sampleIndex]));
            if (sampleIndex % 4u == 0u && std::abs(first[sampleIndex]) > 1.0e-7f)
                nonzero = true;
        }
        ASSERT_TRUE(nonzero) << "the GPU comparison must contain simulated data";
        recovered.InvalidateHistory();
        control.InvalidateHistory();
        SetTemporalInputs(recovered, displacement, 0.15f, true);
        ASSERT_NO_FATAL_FAILURE(ExecuteTemporalDispatch(recovered, frame, other, frameIndex, 3u, 8.0f, true, true));
        EXPECT_EQ(first, sample(recovered));
        SetTemporalInputs(recovered, displacement, 0.0f, false);
        SetTemporalInputs(control, displacement, 0.15f, true);
        ASSERT_NO_FATAL_FAILURE(ExecuteTemporalDispatch(recovered, frame, other, frameIndex, 3u, 8.0f, false, true));
        ASSERT_NO_FATAL_FAILURE(ExecuteTemporalDispatch(control, frame, other, frameIndex, 3u, 8.0f, false, true));
        const auto recoveredValues = sample(recovered);
        EXPECT_EQ(recoveredValues, sample(control));
        EXPECT_NE(recoveredValues, first);
        RecordProperty("temporalResetRecoverySamples", SampleChecksum(recoveredValues));
        SetTemporalInputs(recovered, displacement, 0.15f, true);
        ASSERT_NO_FATAL_FAILURE(ExecuteTemporalDispatch(recovered, frame, other, frameIndex, 3u, 16.0f, true, false));
        EXPECT_EQ(recoveredValues, sample(recovered));
        SetTemporalInputs(recovered, displacement, 0.0f, false);
        SetTemporalInputs(control, displacement, 0.15f, true);
        ASSERT_NO_FATAL_FAILURE(ExecuteTemporalDispatch(recovered, frame, other, frameIndex, 3u, 16.0f, false, false));
        ASSERT_NO_FATAL_FAILURE(ExecuteTemporalDispatch(control, frame, other, frameIndex, 3u, 16.0f, false, false));
        const auto historyRecovery = sample(recovered);
        EXPECT_EQ(historyRecovery, sample(control));
        RecordProperty("temporalHistoryRecoverySamples", SampleChecksum(historyRecovery));
        // Keep the aborted graph and its uploads alive while a newer graph records.
        RenderGraph::RGUploadRing staleUploads(m_Device.get(), 2, 65536);
        RenderGraph::RGFrame stale(m_Device.get(), &persistent, &transient, &staleUploads);
        bool allowStaleSubmission = false;
        SetTemporalInputs(recovered, displacement, 0.15f, true);
        ASSERT_NO_FATAL_FAILURE(ExecuteTemporalDispatch(recovered, stale, other, frameIndex, 3u, 128.0f, true, false,
                                                     &allowStaleSubmission));
        SetTemporalInputs(recovered, displacement, 0.0f, false);
        SetTemporalInputs(control, displacement, 0.15f, true);
        ASSERT_NO_FATAL_FAILURE(ExecuteTemporalDispatch(recovered, frame, other, frameIndex, 3u, 0.0f, false, false));
        ASSERT_NO_FATAL_FAILURE(ExecuteTemporalDispatch(control, frame, other, frameIndex, 3u, 0.0f, false, false));
        const auto beforeStaleRetry = sample(recovered);
        ASSERT_EQ(beforeStaleRetry, sample(control));
        const auto recordedTexture = SimulationTexture(recovered);
        const auto recordedLayout = SimulationLayout(recovered);
        // These new inputs must remain pending when the older graph is refused.
        SetTemporalInputs(recovered, displacement, 0.1f, true);
        SetTemporalInputs(control, displacement, 0.1f, true);
        ASSERT_EQ(recovered.GetPendingSubstepCount(), 2u);
        allowStaleSubmission = true;
        ASSERT_TRUE(m_Device->BeginFrame());
        const auto staleSubmissions = stale.SubmittedFrameCount();
        const auto staleFailures = stale.Stats().PreparationFailures;
        stale.Execute();
        EXPECT_EQ(stale.SubmittedFrameCount(), staleSubmissions);
        EXPECT_GT(stale.Stats().PreparationFailures, staleFailures);
        EXPECT_EQ(SimulationTexture(recovered), recordedTexture);
        EXPECT_EQ(std::memcmp(&SimulationLayout(recovered), &recordedLayout, sizeof(recordedLayout)), 0);
        EXPECT_EQ(recovered.GetPendingSubstepCount(), 2u);
        m_Device->FinalizeFrame();
        m_Device->WaitForIdle();
        EXPECT_EQ(sample(recovered), beforeStaleRetry)
            << "refusing publication after dispatch cannot protect the recorded texture";
        ASSERT_NO_FATAL_FAILURE(ExecuteTemporalDispatch(recovered, frame, other, frameIndex, 2u, 0.0f, false, false));
        ASSERT_NO_FATAL_FAILURE(ExecuteTemporalDispatch(control, frame, other, frameIndex, 2u, 0.0f, false, false));
        EXPECT_EQ(sample(recovered), sample(control))
            << "the refused graph must not consume newer time or impulses";
        recovered.ForgetFrame(stale);
        recovered.ForgetFrame(frame);
        other.BeginFrame(frameIndex++);
        EXPECT_TRUE(recovered.PlanSubsteps(other, 1000.0f, 0.0f).Steps.empty());
        if constexpr (kFoam)
            EXPECT_EQ(recovered.GetFoamTexture(other), SimulationTexture(recovered));
        else
            EXPECT_EQ(recovered.GetTexture(other), SimulationTexture(recovered));
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
                [&query, &frame, prepared, token, params](RenderGraph::RGPassBuilder& pass)
                {
                    pass.UsePipeline(query.GetPipelineId());
                    pass.PreventCulling();
                    pass.Prepare([&query, &frame, prepared, token, buffer = params.Buffer, offset = params.Offset,
                                  values = params.Ptr](RenderGraph::RGPrepareContext& context)
                    {
                        return query.PrepareDispatch(context.GetDevice(), frame, buffer, offset,
                                                     *values, token, *prepared);
                    });
                },
                [&query, prepared](RenderGraph::RGContext& context)
                {
                    EXPECT_TRUE(prepared->Readback.Commands.Pipeline.IsValid());
                    EXPECT_TRUE(prepared->Readback.Commands.Set.IsValid());
                    query.RecordDispatch(context.Cmd, *prepared);
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

    std::vector<float> SampleField(PipelineHandle pipeline, const DescriptorSetLayoutDesc& layout,
                                   TextureHandle texture, SamplerHandle sampler, size_t valueCount,
                                   uint32 groupsX, uint32 groupsY, uint32 groupsZ)
    {
        constexpr float kUnwrittenSample = -17.0f;
        std::vector<float> values(valueCount, kUnwrittenSample);
        const auto output = MakeBuffer(values.size() * sizeof(float), BufferUsage::Storage, values.data());
        EXPECT_TRUE(output.IsValid());
        if (!output.IsValid())
            return values;
        DescriptorSetDesc descriptor{};
        descriptor.layout = layout;
        const auto set = m_Device->CreateDescriptorSet(descriptor);
        EXPECT_TRUE(set.IsValid());
        if (!set.IsValid())
            return values;
        m_Device->UpdateStorageBufferBinding(set, 0, output, 0, values.size() * sizeof(float));
        m_Device->UpdateCombinedImageSamplerBinding(set, 1, texture, sampler);
        auto commands = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        commands->Begin();
        commands->SetPipeline(pipeline);
        commands->BindDescriptorSet(0, set, pipeline);
        commands->Dispatch(groupsX, groupsY, groupsZ);
        commands->Barrier(ResourceBarrier::CreateMemoryBarrier(
            static_cast<uint64>(PipelineStageMask::ComputeShader),
            static_cast<uint64>(PipelineStageMask::Host),
            static_cast<uint64>(ResourceAccessMask::ShaderWrite),
            static_cast<uint64>(ResourceAccessMask::HostRead)));
        commands->End();
        m_Device->ExecuteCommandLists({commands.get()});
        m_Device->WaitForIdle();
        void* mapped = m_Device->MapBuffer(output);
        EXPECT_NE(mapped, nullptr);
        if (mapped)
        {
            std::memcpy(values.data(), mapped, values.size() * sizeof(float));
            m_Device->UnmapBuffer(output);
        }
        m_Device->DestroyDescriptorSet(set);
        return values;
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
