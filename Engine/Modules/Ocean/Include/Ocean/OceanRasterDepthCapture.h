#pragma once

#include "Rendering/Core/Device.h" // DescriptorSetLayoutDesc
#include "Rendering/Core/Handle.h"
#include "Rendering/Core/PipelineIdentifiers.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Core/RenderGraph/RGReadbackRing.h"
#include "Rendering/Core/RenderGraph/RGFrameStamp.h"
#include "Rendering/CameraTypes.h"
#include "Types/Types.h"

#include <cstdint>
#include <mutex>
#include <memory>
#include <vector>

namespace GameEngine::Engine::Renderer::Pipeline
{
struct ViewDeclare;
} // namespace GameEngine::Engine::Renderer::Pipeline

namespace GameEngine::Engine::Renderer
{
class RenderServices;
} // namespace GameEngine::Engine::Renderer

namespace GameEngine::Ocean
{

struct OceanRasterDepthCaptureSettings
{
    bool Enabled = false;
    uint32 Resolution = 512u;
    uint32 RenderLayerMask = 0xFFFFFFFFu;
    float32 SizeX = 512.0f;
    float32 SizeZ = 512.0f;
    float32 TopPadding = 64.0f;
    float32 DeepWaterDepth = 60000.0f;
};

struct OceanRasterDepthSampling
{
    Rendering::TextureHandle Texture;
    Rendering::SamplerHandle Sampler;
    Rendering::RenderGraph::RGTexture Graph;
    float32 OriginX = 0, OriginZ = 0, SizeX = 1, SizeZ = 1, Deep = 60000;
};

// Top-down geometry capture. Sampling begins one world frame after the first
// recorded capture; updating owners sample their planned output in the same graph.
class OceanRasterDepthCapture
{
public:
    OceanRasterDepthCapture();
    ~OceanRasterDepthCapture();

    bool DeclareForView(Engine::Renderer::Pipeline::ViewDeclare& d,
                        float32 centerX, float32 centerZ, float32 seaLevel,
                        const OceanRasterDepthCaptureSettings& settings);
    // The frame retains the selected output generation and its matching footprint.
    bool GetSampling(Rendering::RenderGraph::RGFrame& frame, OceanRasterDepthSampling& out) const;
    Rendering::ViewId GetCaptureViewId() const { return m_ViewId; }
    bool SampleDepth(float32 worldX, float32 worldZ, float32& outDepth) const;
    void InvalidateReadback();

    void OnFrameSubmitted(const Rendering::RenderGraph::RGFrame& frame,
                          const Rendering::IDevice::GpuSyncToken& token)
    {
        m_Readback.OnFrameSubmitted(frame, token);
    }
    void OnFrameAborted(const Rendering::RenderGraph::RGFrame& frame)
    {
        m_Readback.CancelUnsubmitted(frame);
    }
    void OnFrameStreamRetired(const Rendering::RenderGraph::RGFrame& frame);

private:
    friend struct RasterDepthCaptureTestAccess;
    struct OutputGeneration;
    struct Snapshot;
    struct FramePlan;
    bool EnsurePipeline(Rendering::IDevice& device);
    bool EnsureOutput(Rendering::IDevice& device, uint32 resolution);
    bool EnsureReadbackRing(Rendering::IDevice& device, uint32 resolution);
    void EnsureView(Engine::Renderer::RenderServices& rs, const Rendering::ViewDesc& mainView);
    Rendering::CameraData MakeTopDownCamera(float32 centerX, float32 centerZ, float32 seaLevel,
                                             const OceanRasterDepthCaptureSettings& settings) const;
    FramePlan* PlanSnapshot(Rendering::RenderGraph::RGFrame& frame, const Snapshot& snapshot,
                            uint64 epoch, bool convert);
    bool IsCurrentPlan(const FramePlan& plan) const;
    bool PrepareConverter(FramePlan& plan, Rendering::RenderGraph::RGPrepareContext& ctx) const;
    void RecordConverter(FramePlan& plan, Rendering::RenderGraph::RGContext& ctx);
    void ScheduleReadback(Rendering::RenderGraph::RGFrame& frame, FramePlan& plan);
    bool PrepareReadback(FramePlan& plan, Rendering::RenderGraph::RGPrepareContext& ctx);
    void RecordReadback(const FramePlan& plan, Rendering::RenderGraph::RGContext& ctx);
    void ResolveReadyReadback();

    Rendering::IDevice* m_Device = nullptr;
    Engine::Renderer::RenderServices* m_RenderServices = nullptr;
    bool m_PipelineLoadAttempted = false;
    bool m_WarnedLoadFailed = false;
    Rendering::ViewId m_ViewId = 0;
    Rendering::CameraId m_CameraId = 0;
    bool m_ViewAllocated = false;
    bool m_CaptureArmed = false;
    uint64 m_ArmedEpoch = 0;
    uint64 m_ClaimedEpoch = ~0ull;
    uint64 m_CaptureSerial = 0;
    std::shared_ptr<OutputGeneration> m_OutputGeneration;
    std::unique_ptr<Snapshot> m_Committed;
    Rendering::RenderGraph::RGFrameStamp m_PlannedFrame;
    FramePlan* m_Plan = nullptr;
    bool m_ReadbackPending = false;
    Rendering::SamplerHandle m_Sampler;
    Rendering::ComputePipelineId m_PipelineId{};
    Rendering::DescriptorSetLayoutDesc m_Layout{};

    struct ReadbackMeta
    {
        float32 OriginX = 0, OriginZ = 0, SizeX = 1, SizeZ = 1, Deep = 60000;
        uint32 Resolution = 0;
    };
    Rendering::RenderGraph::RGReadbackRing<ReadbackMeta> m_Readback;
    uint32 m_ReadbackResolution = 0;
    mutable std::mutex m_ReadbackMutex;
    std::vector<float32> m_CPUWaterDepth;
    uint32 m_CPUReadbackResolution = 0;
    bool m_HasReadbackData = false;
    float32 m_ReadOriginX = 0, m_ReadOriginZ = 0, m_ReadSizeX = 1, m_ReadSizeZ = 1;
    float32 m_ReadDeepWaterDepth = 60000;
};

} // namespace GameEngine::Ocean
