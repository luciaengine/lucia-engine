#pragma once
#include "Components/Rendering/Ocean.h"
#include "Types/FlatMap.h"
#include "Mathematics/Vector3.h"
#include "Rendering/Core/Device.h"
#include "Rendering/CameraTypes.h"
#include <string>
#include <memory>
namespace GameEngine::Engine::Renderer::Pipeline
{
struct ViewDeclare;
}
namespace GameEngine::Rendering::RenderGraph
{
class RGFrame;
class RGPrepareContext;
class RGContext;
}
namespace GameEngine::Ocean
{
class OceanRenderFeature;
class OceanSprayGPU
{
  public:
    OceanSprayGPU();
    ~OceanSprayGPU();
    void SetSettings(const Components::OceanSurface &surface, float windSpeed, float windDegrees);
    bool Enabled() const
    {
        return m_Surface.Spray;
    }
    void RebaseOrigin(float x, float y, float z);
    bool DeclareForView(Engine::Renderer::Pipeline::ViewDeclare &d, OceanRenderFeature &ocean);
    void ForgetFrame(const Rendering::RenderGraph::RGFrame& frame);

  private:
    bool Initialize(Rendering::IDevice *device);
    struct History;
    struct FramePlan;
    bool IsCurrentPlan(Rendering::ViewId view, const FramePlan& plan) const;
    bool PrepareSimulation(Rendering::ViewId view, FramePlan& plan,
                           Rendering::RenderGraph::RGPrepareContext& ctx) const;
    bool PrepareCull(Rendering::ViewId view, FramePlan& plan,
                     Rendering::RenderGraph::RGPrepareContext& ctx) const;
    bool PrepareDraw(Rendering::ViewId view, FramePlan& plan,
                     Rendering::RenderGraph::RGPrepareContext& ctx) const;
    void RecordSimulation(Rendering::ViewId view, FramePlan& plan, Rendering::RenderGraph::RGContext& ctx);
    void RecordCull(Rendering::ViewId view, FramePlan& plan, Rendering::RenderGraph::RGContext& ctx);
    void RecordDraw(Rendering::ViewId view, FramePlan& plan, Rendering::RenderGraph::RGContext& ctx);
    Rendering::IDevice *m_Device = nullptr;
    Components::OceanSurface m_Surface{};
    float m_WindSpeed = 0, m_WindDegrees = 0;
    Mathematics::Vector3 m_Origin{};
    Rendering::SamplerHandle m_Sampler{};
    bool m_LoadFailed = false;
    Rendering::ComputePipelineId m_Simulate{}, m_Cull{};
    Rendering::GraphicsPipelineId m_Draw{};
    Rendering::DescriptorSetLayoutDesc m_SimLayout{}, m_CullLayout{}, m_DrawLayout{}, m_OceanLayout{};
    FlatMap<Rendering::ViewId, std::shared_ptr<History>> m_History;
};
} // namespace GameEngine::Ocean
