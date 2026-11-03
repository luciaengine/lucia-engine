#pragma once
#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"
#include <memory>

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{
class PlanarReflectionNode final : public IRenderPipelineNode
{
public:
    ~PlanarReflectionNode() override;
    const char* GetTypeName() const override { return "PlanarReflection"; }
    bool Initialize(std::string nodeId, std::string nodeJson, std::string* outError) override;
    void Declare(RenderPipelineInstance& instance, const PipelineDeclareContext& ctx) override;
    void DeclareForView(ViewDeclare& d) override;
    // HDR stitch-through consumers may render in-place (e.g. SunGlare) when
    // SSR is inactive. The composite must support graphics as well as compute.
    static Rendering::TextureDesc CompositeTargetDesc(uint32_t width, uint32_t height);
private:
    void LoadShader(Rendering::IDevice* device);
    Rendering::RenderGraph::RGTexture BlurCapture(ViewDeclare& d,
        Rendering::RenderGraph::RGTexture source, float sigma, uint32_t index);
    Rendering::ComputePipelineId m_BlurPipeline{};
    Rendering::DescriptorSetLayoutDesc m_BlurLayout{};
    std::unique_ptr<Rendering::ShaderMeta> m_BlurMeta;
    std::string m_Input = "SceneColor";
    std::string m_Output = "HDRPlanar";
    Rendering::IDevice* m_Device = nullptr;
    Rendering::ComputePipelineId m_Pipeline{};
    Rendering::DescriptorSetLayoutDesc m_Layout{};
    std::unique_ptr<Rendering::ShaderMeta> m_Meta;
    Rendering::SamplerHandle m_Sampler{};
};
}
