#pragma once

#include "Engine/Rendering/VirtualShadows/VirtualShadowRequestReuse.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/Pipeline/PipelineIdentifiers.h"
#include "Rendering/Materials/Shader/ShaderMeta.h"
#include "Types/FlatMap.h"

namespace GameEngine::Engine::Renderer
{
struct CascadeFrameData;
struct FeatureDeclareContext;
class VirtualShadowRenderFeature;

namespace Pipeline
{
struct ViewDeclare;
}

/// Marks the virtual pages that a view's depth needs, then declares feedback, page raster and
/// the lighting inputs. One instance belongs to one shadow node.
class VirtualShadowPageRequests
{
  public:
    /// The sun inputs of one view. `Cascades` is null when the world has no shadow-casting sun.
    struct Directional
    {
        const CascadeFrameData* Cascades = nullptr;
        uint32_t CascadeResolution = 0; // Conventional cascade size in texels: sets the filter radius.
        float AuthoredShadowDistance = 0.0f;
        float Softness = 1.0f;
    };

    ~VirtualShadowPageRequests();

    /// `compareSampler` is the hardware depth comparison sampler of the conventional shadow maps.
    void DeclareForView(Pipeline::ViewDeclare& view, VirtualShadowRenderFeature& feature,
                        const FeatureDeclareContext& shadows, Rendering::SamplerHandle compareSampler,
                        const Directional& directional);

  private:
    bool EnsurePipeline(Rendering::IDevice& device);

    Rendering::IDevice* m_Device = nullptr;
    Rendering::ComputePipelineId m_Pipeline{};
    Rendering::DescriptorSetLayoutDesc m_Layout{};
    Rendering::ShaderMeta m_Meta{};
    Rendering::SamplerHandle m_Sampler{};
    FlatMap<uint32_t, Pipeline::Nodes::VirtualShadowRequestReuse> m_RequestCaches;
};
} // namespace GameEngine::Engine::Renderer
