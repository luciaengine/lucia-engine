#include <gtest/gtest.h>

#include "Engine/Rendering/FrameOrchestrator.h"
#include "Engine/Rendering/RenderServices.h"
#include "Ocean/OceanDynWavesReadback.h"
#include "Ocean/OceanFFT.h"
#include "Ocean/OceanGPUQuery.h"
#include "Ocean/OceanHeightField.h"
#include "Ocean/OceanRenderFeature.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/BarrierMapping.h"
#include "Rendering/Core/PassPhase.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "TestTempDir.h"

#include <algorithm>
#include <cstring>
#include <type_traits>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;
using namespace GameEngine::Ocean;

namespace
{
void OrderReadbackWrites(CommandList& commands)
{
    commands.Barrier(ResourceBarrier::CreateMemoryBarrier(
        static_cast<uint64>(PipelineStageMask::ComputeShader),
        static_cast<uint64>(PipelineStageMask::ComputeShader),
        static_cast<uint64>(ResourceAccessMask::ShaderWrite),
        static_cast<uint64>(ResourceAccessMask::ShaderRead) | static_cast<uint64>(ResourceAccessMask::ShaderWrite)));
}

template<class Readback>
struct ReadbackReservationProbe
{
    static constexpr bool kHeight = std::is_same_v<Readback, OceanHeightField>;
    using Params = std::conditional_t<kHeight, OceanHeightQueryParamsGPU, OceanDynWavesQueryParamsGPU>;
    Readback& Reader;
    RenderGraph::RGFrame& Frame;
    RenderGraph::RGFrame::TypedUpload<Params> OriginalParams, ReplacementParams;
    TextureHandle OriginalTexture, ReplacementTexture;
    SamplerHandle OriginalSampler, ReplacementSampler;
    OceanPreparedReadbackDispatch Original, Replacement;

    void SetSource(TextureHandle texture, SamplerHandle sampler)
    {
        if constexpr (kHeight)
            Reader.SetFrameInputs(texture, sampler, 1u, 17.0f, 1.0f, 15.0f, 10.0f);
        else
            Reader.SetFrameInputs(texture, sampler, OriginalParams.Ptr->Cascade);
    }

    bool Prepare(RenderGraph::RGPrepareContext& ctx)
    {
        SetSource(OriginalTexture, OriginalSampler);
        if (!Reader.PrepareDispatch(ctx.GetDevice(), Frame, OriginalParams.Buffer,
            OriginalParams.Offset, *OriginalParams.Ptr, Original) || !Original.Commands.IsValid())
            return false;
        SetSource(ReplacementTexture, ReplacementSampler);
        const auto capacity = std::max(1u, ctx.GetDevice()->GetFramesInFlight()) + 2u;
        for (uint32 index = 0; index < capacity; ++index)
        {
            Reader.OnFrameAborted(Frame);
            Replacement = {};
            if (!Reader.PrepareDispatch(ctx.GetDevice(), Frame, ReplacementParams.Buffer,
                ReplacementParams.Offset, *ReplacementParams.Ptr, Replacement) ||
                !Replacement.Commands.IsValid())
                return false;
        }
        EXPECT_EQ(Original.Output, Replacement.Output);
        EXPECT_NE(Original.Reservation, Replacement.Reservation);
        return true;
    }

    void Record(RenderGraph::RGContext& ctx)
    {
        Reader.RecordDispatch(ctx.GetDevice(), ctx.Cmd, Replacement);
        OrderReadbackWrites(*ctx.Cmd);
        Reader.RecordDispatch(ctx.GetDevice(), ctx.Cmd, Original);
    }
};

struct QueryReservationProbe
{
    OceanGPUQuery& Query;
    RenderGraph::RGFrame& Frame;
    RenderGraph::RGFrame::TypedUpload<OceanGPUQueryParamsGPU> OriginalParams, ReplacementParams;
    OceanGPUQueryToken Token = 0;
    OceanPreparedQueryDispatch Original, Replacement;

    bool Prepare(RenderGraph::RGPrepareContext& ctx)
    {
        if (!Query.PrepareDispatch(ctx.GetDevice(), Frame, OriginalParams.Buffer, OriginalParams.Offset,
                                  *OriginalParams.Ptr, Token, Original) || !Original.Readback.Commands.IsValid())
            return false;
        const auto capacity = std::max(1u, ctx.GetDevice()->GetFramesInFlight()) + 2u;
        for (uint32 index = 0; index < capacity; ++index)
        {
            Query.OnFrameAborted(Frame);
            Replacement = {};
            if (!Query.PrepareDispatch(ctx.GetDevice(), Frame, ReplacementParams.Buffer, ReplacementParams.Offset,
                                      *ReplacementParams.Ptr, Token, Replacement) || !Replacement.Readback.Commands.IsValid())
                return false;
        }
        EXPECT_EQ(Original.Readback.Output, Replacement.Readback.Output);
        EXPECT_NE(Original.Readback.Reservation, Replacement.Readback.Reservation);
        return true;
    }

    void Record(RenderGraph::RGContext& ctx)
    {
        Query.RecordDispatch(ctx.Cmd, Replacement);
        EXPECT_FALSE(Query.HasPending());
        OrderReadbackWrites(*ctx.Cmd);
        Query.RecordDispatch(ctx.Cmd, Original);
    }
};

class OceanReadbackPreparationTest : public ::testing::Test
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

    void CreateConstantField(TextureHandle& texture, SamplerHandle& sampler, float red = 0.25f)
    {
        TextureDesc desc{};
        desc.width = desc.height = 16;
        desc.arrayLayers = desc.depth = desc.mipLevels = desc.sampleCount = 1;
        desc.format = static_cast<uint32>(TextureFormat::R32G32B32A32_FLOAT);
        desc.usage = static_cast<uint32>(TextureUsage::ShaderResource | TextureUsage::TransferDst);
        desc.flags = TextureCreateFlags::ForceArrayView;
        texture = m_Device->CreateTexture(desc);
        ASSERT_TRUE(texture.IsValid());
        m_Textures.push_back(texture);
        sampler = m_Device->CreateSampler(SamplerDesc::MaterialLinearClamp("OceanReadbackSource"));
        ASSERT_TRUE(sampler.IsValid());
        m_Samplers.push_back(sampler);
        std::vector<float> texels(16u * 16u * 4u);
        for (size_t i = 0; i < texels.size(); i += 4)
        {
            texels[i] = red;
            texels[i + 1] = 1.5f;
            texels[i + 2] = 0.5f;
        }
        BufferDesc uploadDesc{};
        uploadDesc.size = texels.size() * sizeof(float);
        uploadDesc.usage = static_cast<uint32>(BufferUsage::TransferSrc);
        uploadDesc.memoryUsage = BufferMemoryUsage::Upload;
        const auto upload = m_Device->CreateBuffer(uploadDesc);
        ASSERT_TRUE(upload.IsValid());
        m_Buffers.push_back(upload);
        void* data = m_Device->MapBuffer(upload);
        ASSERT_NE(data, nullptr);
        std::memcpy(data, texels.data(), uploadDesc.size);
        m_Device->UnmapBuffer(upload);
        auto commands = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        commands->Begin();
        commands->Barrier(ResourceBarrier::CreateTextureBarrier(
            texture, ResourceState::Undefined, ResourceState::CopyDest));
        commands->CopyBufferToTextureSubresource(upload, texture, 0, 0, 16, 16);
        commands->Barrier(ResourceBarrier::CreateTextureBarrier(
            texture, ResourceState::CopyDest, ResourceState::ShaderResource));
        commands->End();
        m_Device->ExecuteCommandLists({commands.get()});
        m_Device->WaitForIdle();
    }

    template<class Readback>
    void CheckSnapshotReadback(GraphicsAPI expected)
    {
        ASSERT_EQ(m_Device->GetAPI(), expected);
        constexpr bool kHeight = std::is_same_v<Readback, OceanHeightField>;
        using Params = std::conditional_t<kHeight, OceanHeightQueryParamsGPU, OceanDynWavesQueryParamsGPU>;
        TextureHandle source;
        SamplerHandle sampler;
        ASSERT_NO_FATAL_FAILURE(CreateConstantField(source, sampler));
        Readback readback;
        ASSERT_TRUE(readback.Initialize(m_Device.get()));
        const auto inputs = [&](float camera, float seaLevel)
        {
            readback.BeginFrame(camera, 0.0f);
            if constexpr (kHeight)
                readback.SetFrameInputs(source, sampler, 1u, seaLevel, 1.0f, 15.0f, 10.0f);
            else
            {
                OceanCascadeLayoutGPU layout{};
                layout.LodCount = 1;
                layout.CascadeOriginScale[0][0] = -2048.0f;
                layout.CascadeOriginScale[0][1] = -2048.0f;
                layout.CascadeOriginScale[0][2] = 256.0f;
                layout.CascadeOriginScale[0][3] = 1.0f;
                readback.SetFrameInputs(source, sampler, layout);
            }
        };
        RenderGraph::RGResourcePool persistent(m_Device.get());
        RenderGraph::RGTransientPool transient(m_Device.get());
        RenderGraph::RGUploadRing uploads(m_Device.get(), 2, 65536);
        RenderGraph::RGFrame frame(m_Device.get(), &persistent, &transient, &uploads);
        for (uint32 index = 0; index < 4; ++index)
        {
            SCOPED_TRACE(index);
            ASSERT_TRUE(m_Device->BeginFrame());
            frame.BeginFrame(index);
            inputs(index < 2 ? 0.0f : 1024.0f, 17.0f + 8.0f * index);
            const auto params = frame.AllocUpload<Params>();
            ASSERT_TRUE(params.Valid());
            readback.FillParams(*params.Ptr);
            auto* prepared = frame.AllocatePassData<OceanPreparedReadbackDispatch>();
            const auto witness = frame.AddPass("OceanReadback.FirstRecord", PassPhase::kEarlySetup,
                [](RenderGraph::RGPassBuilder& pass) { pass.PreventCulling(); },
                [&](RenderGraph::RGContext&)
                {
                    EXPECT_TRUE(prepared->Commands.Pipeline.IsValid());
                    EXPECT_TRUE(prepared->Commands.Set.IsValid());
                    inputs(4096.0f, 99.0f);
                });
            const auto input = frame.ImportExternalTexture("OceanReadback.Source", source,
                ResourceState::ShaderResource, TextureFormat::R32G32B32A32_FLOAT, 1, 1);
            const auto writer = frame.AddPass("OceanReadback.Prepared", PassPhase::kEarlySetup,
                [&](RenderGraph::RGPassBuilder& pass)
                {
                    pass.UsePipeline(readback.GetPipelineId());
                    pass.Read(input, RenderGraph::RGTextureRead::SampledCompute);
                    pass.PreventCulling();
                    pass.Prepare([&, prepared, buffer = params.Buffer, offset = params.Offset,
                                  values = params.Ptr](RenderGraph::RGPrepareContext& ctx)
                    {
                        if (!readback.PrepareDispatch(ctx.GetDevice(), frame, buffer, offset, *values, *prepared))
                            return false;
                        const auto output = prepared->Output;
                        const auto reservation = prepared->Reservation;
                        const bool ready = readback.PrepareDispatch(
                            ctx.GetDevice(), frame, buffer, offset, *values, *prepared);
                        EXPECT_EQ(prepared->Output, output);
                        EXPECT_EQ(prepared->Reservation, reservation);
                        return ready;
                    });
                },
                [&, prepared](RenderGraph::RGContext& ctx)
                { readback.RecordDispatch(ctx.GetDevice(), ctx.Cmd, *prepared); });
            frame.AddOrderingEdge(witness, writer);
            if (index == 1)
            {
                const auto abort = frame.AddPass("OceanReadback.Abort", PassPhase::kFinalize,
                    [](RenderGraph::RGPassBuilder& pass)
                    {
                        pass.PreventCulling();
                        pass.Prepare([](RenderGraph::RGPrepareContext&) { return false; });
                    }, [](RenderGraph::RGContext&) {});
                frame.AddOrderingEdge(writer, abort);
            }
            const auto submitted = frame.SubmittedFrameCount();
            frame.Execute();
            EXPECT_TRUE(prepared->Commands.IsValid());
            EXPECT_EQ(frame.SubmittedFrameCount(), submitted + (index == 1 ? 0u : 1u));
            if (index == 1)
            {
                const auto output = prepared->Output;
                const auto reservation = prepared->Reservation;
                frame.Execute();
                EXPECT_EQ(prepared->Output, output);
                EXPECT_EQ(prepared->Reservation, reservation);
                readback.OnFrameAborted(frame);
            }
            else
                readback.OnFrameSubmitted(frame, frame.SubmissionToken());
            m_Device->FinalizeFrame();
            m_Device->WaitForIdle();
            float value = -17.0f;
            if (index < 2)
                EXPECT_FALSE(readback.Sample(0.0f, 0.0f, value));
            else
            {
                const float point = index == 2 ? 0.0f : 1024.0f;
                ASSERT_TRUE(readback.Sample(point, 0.0f, value));
                EXPECT_FLOAT_EQ(value, kHeight ? (index == 2 ? 18.5f : 34.5f) : 0.25f);
            }
        }
    }

    template<class Readback>
    void CheckRevokedReadbackSlot(GraphicsAPI expected)
    {
        ASSERT_EQ(m_Device->GetAPI(), expected);
        constexpr bool kHeight = std::is_same_v<Readback, OceanHeightField>;
        using Params = typename ReadbackReservationProbe<Readback>::Params;
        TextureHandle first, second;
        SamplerHandle firstSampler, secondSampler;
        ASSERT_NO_FATAL_FAILURE(CreateConstantField(first, firstSampler));
        ASSERT_NO_FATAL_FAILURE(CreateConstantField(second, secondSampler, 0.75f));
        Readback readback;
        ASSERT_TRUE(readback.Initialize(m_Device.get()));
        readback.BeginFrame(0.0f, 0.0f);
        if constexpr (kHeight)
            readback.SetFrameInputs(first, firstSampler, 1u, 17.0f, 1.0f, 15.0f, 10.0f);
        else
        {
            OceanCascadeLayoutGPU layout{};
            layout.LodCount = 1u;
            layout.CascadeOriginScale[0][0] = layout.CascadeOriginScale[0][1] = -2048.0f;
            layout.CascadeOriginScale[0][2] = 256.0f;
            layout.CascadeOriginScale[0][3] = 1.0f;
            readback.SetFrameInputs(first, firstSampler, layout);
        }
        RenderGraph::RGResourcePool persistent(m_Device.get());
        RenderGraph::RGTransientPool transient(m_Device.get());
        RenderGraph::RGUploadRing uploads(m_Device.get(), 2u, 65536u);
        RenderGraph::RGFrame frame(m_Device.get(), &persistent, &transient, &uploads);
        ASSERT_TRUE(m_Device->BeginFrame());
        frame.BeginFrame(0u);
        const auto original = frame.AllocUpload<Params>();
        const auto replacement = frame.AllocUpload<Params>();
        ASSERT_TRUE(original.Valid() && replacement.Valid());
        readback.FillParams(*original.Ptr);
        *replacement.Ptr = *original.Ptr;
        if constexpr (kHeight)
            replacement.Ptr->SeaLevel = 37.0f;
        ReadbackReservationProbe<Readback> probe{readback, frame, original, replacement,
            first, second, firstSampler, secondSampler, {}, {}};
        const auto firstInput = frame.ImportExternalTexture("ReadbackABA.First", first,
            ResourceState::ShaderResource, TextureFormat::R32G32B32A32_FLOAT, 1u, 1u);
        const auto secondInput = frame.ImportExternalTexture("ReadbackABA.Second", second,
            ResourceState::ShaderResource, TextureFormat::R32G32B32A32_FLOAT, 1u, 1u);
        frame.AddPass("ReadbackABA.ReassignedSlot", PassPhase::kEarlySetup,
            [&](RenderGraph::RGPassBuilder& pass)
            {
                pass.UsePipeline(readback.GetPipelineId());
                pass.Read(firstInput, RenderGraph::RGTextureRead::SampledCompute);
                pass.Read(secondInput, RenderGraph::RGTextureRead::SampledCompute);
                pass.PreventCulling();
                pass.Prepare([&probe](RenderGraph::RGPrepareContext& ctx) { return probe.Prepare(ctx); });
            }, [&probe](RenderGraph::RGContext& ctx) { probe.Record(ctx); });
        frame.Execute();
        EXPECT_EQ(frame.SubmittedFrameCount(), 1u);
        readback.OnFrameSubmitted(frame, frame.SubmissionToken());
        m_Device->FinalizeFrame();
        m_Device->WaitForIdle();
        const auto* mapped = static_cast<const float*>(m_Device->MapBuffer(probe.Replacement.Output));
        ASSERT_NE(mapped, nullptr);
        const float result = mapped[kHeight ? 3u : 0u];
        m_Device->UnmapBuffer(probe.Replacement.Output);
        EXPECT_FLOAT_EQ(result, kHeight ? 38.5f : 0.75f)
            << "the revoked packet must not overwrite the replacement dispatch's result";
    }

    void CheckRevokedQuerySlot(GraphicsAPI expected)
    {
        ASSERT_EQ(m_Device->GetAPI(), expected);
        TextureHandle source;
        SamplerHandle sampler;
        ASSERT_NO_FATAL_FAILURE(CreateConstantField(source, sampler));
        OceanGPUQuery query;
        ASSERT_TRUE(query.Initialize(m_Device.get()));
        query.SetFrameInputs(source, sampler, {}, {});
        const OceanSurfaceQueryPoint point{12.0f, -8.0f};
        const auto token = query.Enqueue(&point, 1u);
        RenderGraph::RGResourcePool persistent(m_Device.get());
        RenderGraph::RGTransientPool transient(m_Device.get());
        RenderGraph::RGUploadRing uploads(m_Device.get(), 2u, 65536u);
        RenderGraph::RGFrame frame(m_Device.get(), &persistent, &transient, &uploads);
        ASSERT_TRUE(m_Device->BeginFrame());
        frame.BeginFrame(0u);
        OceanParamsGPU ocean{};
        ocean.SeaLevel = 17.0f;
        ocean.FFTCascadeCount = 0u;
        EXPECT_EQ(query.PrepareNext(ocean, {}, false), token);
        const auto original = frame.AllocUpload<OceanGPUQueryParamsGPU>();
        const auto replacement = frame.AllocUpload<OceanGPUQueryParamsGPU>();
        ASSERT_TRUE(original.Valid() && replacement.Valid());
        query.FillParams(*original.Ptr);
        ocean.SeaLevel = 37.0f;
        EXPECT_EQ(query.PrepareNext(ocean, {}, false), token);
        query.FillParams(*replacement.Ptr);
        QueryReservationProbe probe{query, frame, original, replacement, token, {}, {}};
        const auto input = frame.ImportExternalTexture("QueryABA.Source", source,
            ResourceState::ShaderResource, TextureFormat::R32G32B32A32_FLOAT, 1u, 1u);
        frame.AddPass("QueryABA.ReassignedSlot", PassPhase::kEarlySetup,
            [&](RenderGraph::RGPassBuilder& pass)
            {
                pass.UsePipeline(query.GetPipelineId());
                pass.Read(input, RenderGraph::RGTextureRead::SampledCompute);
                pass.PreventCulling();
                pass.Prepare([&probe](RenderGraph::RGPrepareContext& ctx) { return probe.Prepare(ctx); });
            }, [&probe](RenderGraph::RGContext& ctx) { probe.Record(ctx); });
        frame.Execute();
        EXPECT_EQ(frame.SubmittedFrameCount(), 1u);
        query.OnFrameSubmitted(frame, frame.SubmissionToken());
        m_Device->FinalizeFrame();
        m_Device->WaitForIdle();
        query.PollCompleted(m_Device.get());
        std::vector<OceanSurfaceSample> samples;
        ASSERT_TRUE(query.TryTake(token, samples));
        ASSERT_EQ(samples.size(), 1u);
        EXPECT_FLOAT_EQ(samples[0].Height, 37.0f)
            << "a newer reservation must not publish the revoked packet's height";
    }

    void CheckQueryCancellationAndBackPressure(GraphicsAPI expected)
    {
        ASSERT_EQ(m_Device->GetAPI(), expected);
        Engine::Renderer::RenderServices services;
        ASSERT_TRUE(services.Initialize(m_Device.get()));
        auto& query = services.EnsureFeature<OceanRenderFeature>().GetGPUQuery();
        ASSERT_TRUE(query.Initialize(m_Device.get()));
        OceanFFT fft;
        ASSERT_TRUE(fft.Initialize(m_Device.get()));
        query.SetFrameInputs(fft.GetDisplacementTexture(), fft.GetSampler(), {}, {});
        RenderGraph::RGResourcePool persistent(m_Device.get());
        RenderGraph::RGTransientPool transient(m_Device.get());
        RenderGraph::RGUploadRing uploads(m_Device.get(), 2, 65536);
        RenderGraph::RGFrame abandoned(m_Device.get(), &persistent, &transient, &uploads);
        RenderGraph::RGUploadRing otherUploads(m_Device.get(), 2, 65536);
        RenderGraph::RGFrame active(m_Device.get(), &persistent, &transient, &otherUploads);
        const auto execute = [&](RenderGraph::RGFrame& frame, uint32 index, bool abort,
                                 bool expectDispatch, OceanGPUQueryToken token, bool revoke = false)
        {
            ASSERT_TRUE(m_Device->BeginFrame());
            frame.BeginFrame(index);
            OceanParamsGPU ocean{};
            ocean.SeaLevel = 17.0f;
            ocean.FFTCascadeCount = 0;
            EXPECT_EQ(query.PrepareNext(ocean, {}, false), token);
            const auto params = frame.AllocUpload<OceanGPUQueryParamsGPU>();
            ASSERT_TRUE(params.Valid());
            query.FillParams(*params.Ptr);
            auto* prepared = frame.AllocatePassData<OceanPreparedQueryDispatch>();
            const auto writer = frame.AddPass("OceanQuery.Reservation", PassPhase::kEarlySetup,
                [&](RenderGraph::RGPassBuilder& pass)
                {
                    pass.UsePipeline(query.GetPipelineId());
                    pass.PreventCulling();
                    pass.Prepare([&, prepared, buffer = params.Buffer, offset = params.Offset,
                                  values = params.Ptr](RenderGraph::RGPrepareContext& ctx)
                    {
                        return query.PrepareDispatch(ctx.GetDevice(), frame, buffer, offset,
                                                     *values, token, *prepared);
                    });
                },
                [&, prepared](RenderGraph::RGContext& ctx)
                {
                    if (revoke)
                        query.OnFrameAborted(frame);
                    query.RecordDispatch(ctx.Cmd, *prepared);
                });
            if (abort)
            {
                const auto failure = frame.AddPass("OceanQuery.Abort", PassPhase::kFinalize,
                    [](RenderGraph::RGPassBuilder& pass)
                    {
                        pass.PreventCulling();
                        pass.Prepare([](RenderGraph::RGPrepareContext&) { return false; });
                    }, [](RenderGraph::RGContext&) {});
                frame.AddOrderingEdge(writer, failure);
            }
            const auto submitted = frame.SubmittedFrameCount();
            frame.Execute();
            EXPECT_EQ(prepared->Readback.Commands.IsValid(), expectDispatch);
            EXPECT_EQ(frame.SubmittedFrameCount(), submitted + (abort ? 0u : 1u));
            if (abort)
            {
                const auto canceled = prepared->Readback.Reservation;
                Engine::Renderer::NotifyRenderGraphExecution(frame, submitted, &services.Spine());
                frame.Execute();
                EXPECT_NE(prepared->Readback.Reservation, canceled);
                EXPECT_TRUE(prepared->Readback.Commands.IsValid());
                Engine::Renderer::NotifyRenderGraphExecution(frame, submitted, &services.Spine());
            }
            else
                query.OnFrameSubmitted(frame, frame.SubmissionToken());
            EXPECT_EQ(query.HasPending(), abort || !expectDispatch || revoke);
            m_Device->FinalizeFrame();
            m_Device->WaitForIdle();
        };
        const OceanSurfaceQueryPoint point{12.0f, -8.0f};
        const auto first = query.Enqueue(&point, 1u);
        ASSERT_NO_FATAL_FAILURE(execute(abandoned, 0u, true, true, first));
        const uint32 capacity = std::max(1u, m_Device->GetFramesInFlight()) + 2u;
        std::vector<OceanGPUQueryToken> tokens;
        for (uint32 index = 0; index < capacity; ++index)
        {
            const auto token = index == 0 ? first : query.Enqueue(&point, 1u);
            tokens.push_back(token);
            ASSERT_NO_FATAL_FAILURE(execute(active, index + 1u, false, true, token));
        }
        const auto blocked = query.Enqueue(&point, 1u);
        ASSERT_NO_FATAL_FAILURE(execute(active, capacity + 1u, false, false, blocked));
        query.PollCompleted(m_Device.get());
        for (auto token : tokens)
        {
            std::vector<OceanSurfaceSample> samples;
            ASSERT_TRUE(query.TryTake(token, samples));
            ASSERT_EQ(samples.size(), 1u);
            EXPECT_FLOAT_EQ(samples[0].Height, 17.0f);
        }
        ASSERT_NO_FATAL_FAILURE(execute(active, capacity + 2u, false, true, blocked));
        query.PollCompleted(m_Device.get());
        std::vector<OceanSurfaceSample> samples;
        ASSERT_TRUE(query.TryTake(blocked, samples));
        ASSERT_EQ(samples.size(), 1u);
        EXPECT_FLOAT_EQ(samples[0].Height, 17.0f);
        const auto revoked = query.Enqueue(&point, 1u);
        ASSERT_NO_FATAL_FAILURE(execute(active, capacity + 3u, false, true, revoked, true));
        query.PollCompleted(m_Device.get());
        EXPECT_FALSE(query.TryTake(revoked, samples));
        ASSERT_NO_FATAL_FAILURE(execute(active, capacity + 4u, false, true, revoked));
        query.PollCompleted(m_Device.get());
        ASSERT_TRUE(query.TryTake(revoked, samples));
        ASSERT_EQ(samples.size(), 1u);
        EXPECT_FLOAT_EQ(samples[0].Height, 17.0f);
        services.Shutdown();
    }

    std::unique_ptr<IDevice> m_Device;
    std::vector<BufferHandle> m_Buffers;
    std::vector<TextureHandle> m_Textures;
    std::vector<SamplerHandle> m_Samplers;
    TestUtils::ScopedTempDir m_Cache{TestUtils::MakeUniqueTempDirectory("ocean_readback_preparation")};
};
} // namespace

TEST_F(OceanReadbackPreparationTest, RevokedHeightPacketCannotOverwriteAReassignedSlot)
{
    CheckRevokedReadbackSlot<OceanHeightField>(GraphicsAPI::Vulkan);
}
TEST_F(OceanReadbackPreparationTest, RevokedDynamicPacketCannotOverwriteAReassignedSlot)
{
    CheckRevokedReadbackSlot<OceanDynWavesReadback>(GraphicsAPI::Vulkan);
}
TEST_F(OceanReadbackPreparationTest, RevokedQueryPacketCannotOverwriteAReassignedSlot)
{
    CheckRevokedQuerySlot(GraphicsAPI::Vulkan);
}
TEST_F(OceanReadbackPreparationTest, HeightReadbackPreparesAndRetainsTheWrittenOrigin)
{
    CheckSnapshotReadback<OceanHeightField>(GraphicsAPI::Vulkan);
}
TEST_F(OceanReadbackPreparationTest, DynamicReadbackPreparesAndRetainsTheWrittenOrigin)
{
    CheckSnapshotReadback<OceanDynWavesReadback>(GraphicsAPI::Vulkan);
}
TEST_F(OceanReadbackPreparationTest, AbortedWindowReleasesItsQuerySlotAndFullRingsSkip)
{
    CheckQueryCancellationAndBackPressure(GraphicsAPI::Vulkan);
}

#if defined(__APPLE__)
class MetalOceanReadbackPreparationTest : public OceanReadbackPreparationTest
{
  protected:
    GraphicsAPI RequestedAPI() const override { return GraphicsAPI::Metal; }
};
TEST_F(MetalOceanReadbackPreparationTest, RevokedHeightPacketCannotOverwriteAReassignedSlot)
{
    CheckRevokedReadbackSlot<OceanHeightField>(GraphicsAPI::Metal);
}
TEST_F(MetalOceanReadbackPreparationTest, RevokedDynamicPacketCannotOverwriteAReassignedSlot)
{
    CheckRevokedReadbackSlot<OceanDynWavesReadback>(GraphicsAPI::Metal);
}
TEST_F(MetalOceanReadbackPreparationTest, RevokedQueryPacketCannotOverwriteAReassignedSlot)
{
    CheckRevokedQuerySlot(GraphicsAPI::Metal);
}
TEST_F(MetalOceanReadbackPreparationTest, HeightReadbackPreparesAndRetainsTheWrittenOrigin)
{
    CheckSnapshotReadback<OceanHeightField>(GraphicsAPI::Metal);
}
TEST_F(MetalOceanReadbackPreparationTest, DynamicReadbackPreparesAndRetainsTheWrittenOrigin)
{
    CheckSnapshotReadback<OceanDynWavesReadback>(GraphicsAPI::Metal);
}
TEST_F(MetalOceanReadbackPreparationTest, AbortedWindowReleasesItsQuerySlotAndFullRingsSkip)
{
    CheckQueryCancellationAndBackPressure(GraphicsAPI::Metal);
}
#endif
