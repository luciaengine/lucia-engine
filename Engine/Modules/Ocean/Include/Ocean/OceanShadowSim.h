#pragma once

#include "Ocean/OceanCascadeArray.h"
#include "Ocean/OceanInputDrawSource.h"
#include "Ocean/OceanSettingsAsset.h"
#include "Rendering/CameraTypes.h"
#include "Rendering/Core/RenderGraph/RGFrameStamp.h"
#include "Types/FlatMap.h"

#include <memory>
#include <vector>

namespace GameEngine::Engine::Renderer::Pipeline
{
struct ViewDeclare;
}
namespace GameEngine::Rendering::RenderGraph
{
class RGPrepareContext;
class RGContext;
}
namespace GameEngine::Ocean
{
// Per-view histories: each engine view has its own directional shadow camera.
struct alignas(16) OceanShadowSamplingGPU
{
    OceanCascadeLayoutGPU Layout{};
    float Channels[4]{}; // hard scale, soft scale, available, pad
};

class OceanShadowSim
{
  public:
    OceanShadowSim();
    ~OceanShadowSim();
    void SetSettings(const OceanShadowSettings &settings)
    {
        m_Settings = settings;
    }
    void SetInputs(std::vector<OceanInputDrawPacket> inputs)
    {
        m_Inputs = std::move(inputs);
    }
    void RebaseOrigin(float x, float z);
    bool DeclareForView(Engine::Renderer::Pipeline::ViewDeclare &d, const OceanParamsGPU &params);
    // Sampling is available only to the graph that declared this view.
    bool FillSampling(Rendering::ViewId view, const Rendering::RenderGraph::RGFrame& frame,
                      OceanShadowSamplingGPU &out) const;
    // The no-frame accessor exposes recorded history; the owner sees its planned output.
    Rendering::TextureHandle GetTexture(Rendering::ViewId view) const;
    Rendering::TextureHandle GetTexture(Rendering::ViewId view,
                                        const Rendering::RenderGraph::RGFrame& frame) const;
    void ForgetFrame(const Rendering::RenderGraph::RGFrame& frame);
    Rendering::SamplerHandle GetSampler() const
    {
        return m_Sampler;
    }
    Rendering::RenderGraph::RGTexture ImportRG(Rendering::RenderGraph::RGFrame &frame,
                                               Rendering::ViewId view) const;

  private:
    struct History;
    struct FramePlan;
    const FramePlan* FindPlan(Rendering::ViewId view, const Rendering::RenderGraph::RGFrame& frame) const;
    bool IsCurrentPlan(Rendering::ViewId view, const FramePlan& plan) const;
    bool Prepare(Rendering::ViewId view, FramePlan& plan, Rendering::RenderGraph::RGPrepareContext& ctx) const;
    void Record(Rendering::ViewId view, FramePlan& plan, Rendering::RenderGraph::RGContext& ctx);
    bool Initialize(Rendering::IDevice *device);
    Rendering::IDevice *m_Device = nullptr;
    Rendering::ComputePipelineId m_Pipeline{};
    Rendering::DescriptorSetLayoutDesc m_Layout{};
    Rendering::SamplerHandle m_Sampler{};
    bool m_LoadFailed = false;
    OceanShadowSettings m_Settings{};
    std::vector<OceanInputDrawPacket> m_Inputs;
    FlatMap<Rendering::ViewId, std::shared_ptr<History>> m_Histories;
};
} // namespace GameEngine::Ocean
