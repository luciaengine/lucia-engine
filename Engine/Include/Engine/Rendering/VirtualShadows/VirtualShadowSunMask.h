#pragma once

#include "Engine/Rendering/CameraAspectRatio.h"
#include "Engine/Rendering/Pipeline/PipelineFrameResources.h"
#include "Mathematics/Vector3.h"
#include "Rendering/Core/Pipeline/PipelineIdentifiers.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"

#include <memory>

namespace GameEngine::Rendering
{
class IDevice;
struct CameraData;
struct ShaderMeta;
} // namespace GameEngine::Rendering

namespace GameEngine::Engine::Renderer
{
struct VirtualShadowViewInputs;

/// What one view's sun mask pass reads.
struct VirtualShadowSunMaskRequest
{
    uint32_t ViewId = 0;
    /// The single-sample prepass depth that the world pass tests its receivers against.
    Rendering::RenderGraph::RGTexture Depth{};
    /// The jittered camera that the prepass rasterized with.
    const Rendering::CameraData* Camera = nullptr;
    Mathematics::Vector3 TowardLight{};
    const VirtualShadowViewInputs* Inputs = nullptr;
    /// The view's ShadowData block, the directional constants of the forward lighting.
    Pipeline::PipelineBufferBindingRG ShadowData{};
    ViewLetterbox Letterbox{};
};

/// The compute pass that resolves the sun's virtual shadow page lookup once per
/// prepass receiver into an R32_UINT mask (Includes/virtual_shadow_sun_mask.glsl).
/// Owns the pipeline and the sampler. The frame owns the per-view mask.
class VirtualShadowSunMask
{
  public:
    explicit VirtualShadowSunMask(Rendering::IDevice* device);
    ~VirtualShadowSunMask();
    VirtualShadowSunMask(const VirtualShadowSunMask&) = delete;
    VirtualShadowSunMask& operator=(const VirtualShadowSunMask&) = delete;

    /// After a device rebuild the sampler died with the old device: forget it.
    void OnDeviceRebuilt(Rendering::IDevice* device);

    /// Declares the mask pass and returns the mask, or an invalid texture when the
    /// pass cannot run this frame (the world pass then keeps the lookup in the shader).
    Rendering::RenderGraph::RGTexture Declare(Rendering::RenderGraph::RGFrame& frame,
                                              const VirtualShadowSunMaskRequest& request);

  private:
    bool Load();

    Rendering::IDevice* m_Device = nullptr;
    Rendering::ComputePipelineId m_Pipeline{};
    std::unique_ptr<Rendering::ShaderMeta> m_Meta;
    Rendering::DescriptorSetLayoutDesc m_Layout{};
    Rendering::SamplerHandle m_Sampler{};
    bool m_LoggedLoadFailure = false;
};
} // namespace GameEngine::Engine::Renderer
