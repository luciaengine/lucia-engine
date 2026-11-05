#pragma once

#include "Ocean/OceanQuery.h"
#include "Ocean/OceanPreparedDispatch.h"
#include "Ocean/OceanTypes.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/Handle.h"
#include "Rendering/Core/PipelineIdentifiers.h"
#include "Rendering/Core/RenderGraph/RGReadbackRing.h"

#include <atomic>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace GameEngine::Rendering
{
class CommandList;
}

namespace GameEngine::Ocean
{

inline constexpr uint32 kMaxOceanGPUQueryPointsPerDispatch = 256u;
using OceanGPUQueryToken = uint64;

struct OceanPreparedQueryDispatch
{
    OceanPreparedReadbackDispatch Readback;
    OceanGPUQueryToken Token = 0;
};

struct OceanGPUQueryParamsGPU
{
    uint32 PointCount = 0u;
    uint32 FFTCascadeCount = 0u;
    uint32 CombinedAvailable = 0u;
    uint32 _Pad0 = 0u;

    float32 SeaLevel = 0.0f;
    float32 Weight = 1.0f;
    float32 MaxHorizontal = 15.0f;
    float32 MaxVertical = 10.0f;

    float32 WaveOriginOffsetX = 0.0f;
    float32 WaveOriginOffsetZ = 0.0f;
    float32 NormalStep = 0.25f;
    float32 _Pad1 = 0.0f;

    OceanCascadeLayoutGPU CombinedCascade;
    float32 Points[kMaxOceanGPUQueryPointsPerDispatch][4] = {};
};
static_assert(sizeof(OceanGPUQueryParamsGPU) ==
                  48 + sizeof(OceanCascadeLayoutGPU) +
                      kMaxOceanGPUQueryPointsPerDispatch * 16u,
              "OceanGPUQueryParamsGPU must match ocean_gpu_query.comp");

// Arbitrary world-point FFT/combined-cascade queries. Requests are queued from
// extraction/physics, one bounded batch is dispatched per render frame, and a
// token-signaled readback ring publishes results without stalling either thread.
class OceanGPUQuery
{
public:
    ~OceanGPUQuery() { m_Readback.Destroy(m_Device); }

    bool Initialize(::GameEngine::Rendering::IDevice* device);
    bool IsReady() const { return m_Ready; }

    OceanGPUQueryToken Enqueue(const OceanSurfaceQueryPoint* points, uint32 count,
                               float32 minimumSpatialLength = 0.0f);
    bool HasPending() const;
    OceanGPUQueryToken PrepareNext(const OceanParamsGPU& oceanParams,
                     const OceanCascadeLayoutGPU& combinedLayout,
                     bool combinedAvailable);
    void FillParams(OceanGPUQueryParamsGPU& out) const { out = m_Params; }

    void SetFrameInputs(::GameEngine::Rendering::TextureHandle fft,
                        ::GameEngine::Rendering::SamplerHandle fftSampler,
                        ::GameEngine::Rendering::TextureHandle combined,
                        ::GameEngine::Rendering::SamplerHandle combinedSampler);
    /// Declare this ID with RGPassBuilder::UsePipeline before preparation.
    ::GameEngine::Rendering::ComputePipelineId GetPipelineId() const { return m_Pipeline; }

    /// Render-thread preparation reserves a frame-owned slot without consuming pending work.
    /// Keep the parameter upload alive through recording; notify submission or abort afterward.
    bool PrepareDispatch(Rendering::IDevice* device, Rendering::RenderGraph::RGFrame& frame,
                         Rendering::BufferHandle paramsBuffer, uint64 paramsOffset,
                         const OceanGPUQueryParamsGPU& params, OceanGPUQueryToken token,
                         OceanPreparedQueryDispatch& prepared);
    /// Render-thread recording validates the reservation before writing its physical slot.
    void RecordDispatch(Rendering::CommandList* commands, const OceanPreparedQueryDispatch& prepared);
    /// Render-thread notification releases this frame's reservations that were not submitted.
    void OnFrameAborted(const Rendering::RenderGraph::RGFrame& frame)
    {
        m_Readback.CancelUnsubmitted(frame);
    }
    void PollCompleted(::GameEngine::Rendering::IDevice* device);
    bool TryTake(OceanGPUQueryToken token, std::vector<OceanSurfaceSample>& out);
    void Clear();

    void OnFrameSubmitted(const ::GameEngine::Rendering::RenderGraph::RGFrame& frame,
                          const ::GameEngine::Rendering::IDevice::GpuSyncToken& token)
    {
        m_Readback.OnFrameSubmitted(frame, token);
    }
    void OnFrameStreamRetired(const ::GameEngine::Rendering::RenderGraph::RGFrame& frame)
    {
        m_Readback.OnFrameStreamRetired(frame);
    }

private:
    struct Request
    {
        OceanGPUQueryToken Token = 0u;
        float32 MinimumSpatialLength = 0.0f;
        std::vector<OceanSurfaceQueryPoint> Points;
    };
    struct ReadbackMeta
    {
        OceanGPUQueryToken Token = 0u;
        uint32 Count = 0u;
        float32 Time = 0.0f;
    };
    struct ResultGPU
    {
        float32 Position[4] = {};
        float32 Normal[4] = {};
        float32 Displacement[4] = {};
    };

    ::GameEngine::Rendering::IDevice* m_Device = nullptr;
    bool m_Ready = false;
    ::GameEngine::Rendering::ComputePipelineId m_Pipeline{};
    ::GameEngine::Rendering::DescriptorSetLayoutDesc m_Layout;
    ::GameEngine::Rendering::RenderGraph::RGReadbackRing<ReadbackMeta> m_Readback;

    mutable std::mutex m_Mutex;
    std::deque<Request> m_Pending;
    std::unordered_map<OceanGPUQueryToken, std::vector<OceanSurfaceSample>> m_Completed;
    std::atomic<OceanGPUQueryToken> m_NextToken{1u};
    Request m_Active;
    OceanGPUQueryParamsGPU m_Params{};

    ::GameEngine::Rendering::TextureHandle m_FFT;
    ::GameEngine::Rendering::SamplerHandle m_FFTSampler;
    ::GameEngine::Rendering::TextureHandle m_Combined;
    ::GameEngine::Rendering::SamplerHandle m_CombinedSampler;
};

} // namespace GameEngine::Ocean
