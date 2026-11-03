#include "Engine/Rendering/Pipeline/Nodes/PlanarReflectionNode.h"
#include "Engine/Rendering/PlanarReflectionFeature.h"
#include "Engine/Rendering/CameraAspectRatio.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/Pipeline/Nodes/PipelineNodeUtils.h"
#include "Engine/Rendering/Pipeline/PipelineResourceNames.h"
#include "Engine/Rendering/Pipeline/RenderPipeline.h"
#include "Logger/Logger.h"
#include "Rendering/Core/NamedDescriptorWriter.h"
#include "Rendering/Materials/MaterialHelper.h"
#include <nlohmann/json.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <cstring>
#include <array>

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{
using namespace ::GameEngine::Rendering;
namespace RG = ::GameEngine::Rendering::RenderGraph;
namespace
{
void LoadPlanarShader(IDevice* device, const char* path, ComputePipelineId& pipeline,
                      std::unique_ptr<ShaderMeta>& meta, DescriptorSetLayoutDesc& layout)
{
    if (pipeline.IsValid()) return;
    ShaderPackage package;
    std::string error;
    if (!LoadShaderPkg(path, device->PreferredShaderSource(), package, &error))
    {
        LOG_WARNING("PlanarReflection: shader unavailable: {}", error);
        return;
    }
    const auto cs = package.stageBytes.find("cs");
    if (cs == package.stageBytes.end() || cs->second.empty()) return;
    meta = std::make_unique<ShaderMeta>(std::move(package.meta));
    ComputePipelineDesc desc{};
    desc.ComputeShader = std::make_shared<const std::vector<uint8_t>>(std::move(cs->second));
    desc.DebugName = path;
    MaterialHelper::ApplyShaderMetaToComputeDesc(*device, *meta, desc,
        MaterialBuilder::MergeMode::Auto, {true, 128},
        [&](uint32_t set, DescriptorSetLayoutDesc& value) { if (set == 0) layout = value; }, &error);
    pipeline = device->InternComputePipeline(std::move(desc));
}
} // namespace

PlanarReflectionNode::~PlanarReflectionNode()
{
    if (m_Device && m_Sampler.IsValid()) m_Device->DestroySampler(m_Sampler);
}
bool PlanarReflectionNode::Initialize(std::string, std::string nodeJson, std::string* outError)
{
    try
    {
        const auto j = nlohmann::json::parse(nodeJson);
        m_Input = j.value("input", m_Input);
        m_Output = j.value("output", m_Output);
        return true;
    }
    catch (const std::exception& e)
    {
        if (outError) *outError = e.what();
        return false;
    }
}
void PlanarReflectionNode::Declare(RenderPipelineInstance& instance, const PipelineDeclareContext&)
{
    LoadShader(instance.GetRenderServices().GetDevice());
}
void PlanarReflectionNode::LoadShader(IDevice* device)
{
    if (!device) return;
    m_Device = device;

    LoadPlanarShader(device, "Shaders/planar_reflection_composite.shaderpkg", m_Pipeline, m_Meta, m_Layout);
    LoadPlanarShader(device, "Shaders/planar_reflection_blur.shaderpkg", m_BlurPipeline, m_BlurMeta, m_BlurLayout);
    if (!m_Sampler.IsValid())
        m_Sampler = device->CreateSampler(SamplerDesc::MaterialLinearClamp("PlanarReflection.Clamp"));
}
RG::RGTexture PlanarReflectionNode::BlurCapture(ViewDeclare& d, RG::RGTexture source,
                                               float sigma, uint32_t index)
{
    if (sigma <= 0.0f || !m_BlurPipeline.IsValid()) return source;
    const auto resource = d.Frame.Graph().ResourceDesc(source.Id);
    TextureDesc desc{};
    desc.width = resource.Width; desc.height = resource.Height;
    desc.depth = 1; desc.arrayLayers = 1; desc.mipLevels = 1; desc.sampleCount = 1;
    desc.format = static_cast<uint32_t>(TextureFormat::R16G16B16A16_FLOAT);
    desc.usage = static_cast<uint32_t>(TextureUsage::ShaderResource | TextureUsage::UnorderedAccess);
    const auto kernel = MakePlanarReflectionBlurKernel(sigma);
    struct Params { float AxisAndCount[4]; glm::vec4 Taps[7]; };
    auto paramsX = d.Frame.AllocUpload<Params>();
    auto paramsY = d.Frame.AllocUpload<Params>();
    if (!paramsX.Valid() || !paramsY.Valid()) return source;
    const auto params = std::array{paramsX, paramsY};
    RG::RGTexture outputs[2];
    const auto prefix = "Blur" + std::to_string(index);
    for (uint32_t axis = 0; axis < 2; ++axis)
    {
        const auto name = d.PassName((prefix + (axis == 0 ? ".Horizontal" : ".Vertical")).c_str());
        outputs[axis] = d.Frame.CreateTexture(name.c_str(), desc);
        if (!outputs[axis].IsValid()) return source;
        params[axis].Ptr->AxisAndCount[0] = axis == 0 ? 1.0f / desc.width : 0.0f;
        params[axis].Ptr->AxisAndCount[1] = axis == 1 ? 1.0f / desc.height : 0.0f;
        params[axis].Ptr->AxisAndCount[2] = static_cast<float>(kernel.Count);
        params[axis].Ptr->AxisAndCount[3] = 0;
        std::memcpy(params[axis].Ptr->Taps, kernel.Taps, sizeof(kernel.Taps));
    }
    for (uint32_t axis = 0; axis < 2; ++axis)
    {
        const auto input = axis == 0 ? source : outputs[0];
        const auto output = outputs[axis];
        const auto name = d.PassName((prefix + (axis == 0 ? ".Horizontal" : ".Vertical")).c_str());
        d.Frame.AddPass(name.c_str(), PassPhase::kDefault,
            [&](RG::RGPassBuilder& p)
            {
                p.Read(input, RG::RGTextureRead::SampledCompute);
                p.Write(output, RG::RGTextureWrite::Storage);
            },
            [=, this](RG::RGContext& ctx)
            {
                auto* device = ctx.GetDevice();
                if (!device || !ctx.Cmd) return;
                const auto pipeline = ctx.GetOrCreatePipelineVariant(m_BlurPipeline);
                if (!pipeline.IsValid()) return;
                DescriptorSetDesc sd{};
                sd.layout = m_BlurLayout; sd.transient = true; sd.debugName = "PlanarReflection.Blur.Set0";
                const auto set = device->CreateDescriptorSet(sd);
                NamedDescriptorWriter writer(device, set, *m_BlurMeta, 0);
                writer.AddCombinedImageSampler("uSource", ctx.GetTexture(input), m_Sampler);
                writer.AddUniformBuffer("BlurParams", params[axis].Buffer,
                    params[axis].Offset, sizeof(Params));
                writer.Flush();
                Detail::BindStorageImageByName(device, set, *m_BlurMeta, "uOutput", ctx.GetTexture(output));
                ctx.Cmd->SetPipeline(pipeline);
                ctx.Cmd->BindDescriptorSet(0, set, pipeline);
                ctx.Cmd->Dispatch((desc.width + 7u) / 8u, (desc.height + 7u) / 8u, 1);
            });
    }
    return outputs[1];
}
TextureDesc PlanarReflectionNode::CompositeTargetDesc(uint32_t width, uint32_t height)
{
    TextureDesc desc{};
    desc.width = width; desc.height = height;
    desc.depth = 1; desc.arrayLayers = 1; desc.mipLevels = 1; desc.sampleCount = 1;
    desc.format = static_cast<uint32_t>(TextureFormat::R16G16B16A16_FLOAT);
    desc.usage = static_cast<uint32_t>(TextureUsage::ShaderResource |
        TextureUsage::UnorderedAccess | TextureUsage::RenderTarget);
    return desc;
}
void PlanarReflectionNode::DeclareForView(ViewDeclare& d)
{
    auto input = d.ResolveTexture(m_Input);
    if (input.IsValid()) d.PublishTexture(m_Output, input);
    auto* feature = d.Services.GetFeature<PlanarReflectionFeature>();
    if (!input.IsValid() || !feature || !m_Pipeline.IsValid() || !m_Sampler.IsValid()) return;
    auto planes = feature->GetPlanes(d.View);
    if (planes.empty()) return;
    const auto normal = d.ResolveTexture(Names::View::NormalRoughnessWritten);
    const auto weight = d.ResolveTexture(Names::View::SSRSpecularWeightWritten);
    const auto radiance = d.ResolveTexture(Names::View::SSRSpecularRadianceWritten);
    const auto depth = d.ViewDepthResolved;
    const auto viewParams = d.ResolveBuffer(Names::Res::ViewParams);
    if (!normal.IsValid() || !weight.IsValid() || !radiance.IsValid() || !depth.IsValid() ||
        !viewParams.IsValid() || !d.RenderWidth || !d.RenderHeight) return;
    RG::RGTexture mask{};
    const auto desc = CompositeTargetDesc(d.RenderWidth, d.RenderHeight);
    struct Params
    {
        float ReflectionViewProj[16];
        float WorldToLocal[16];
        float Plane[4];
        float Settings[4];
        float Flags[4];
        float Viewport[4];
    };
    const auto viewport = ComputeViewportRect(
        d.Services.Views().GetViewLetterbox(d.View.id), d.RenderWidth, d.RenderHeight);
    uint32_t index = 0;
    for (const auto& plane : planes)
    {
        const auto capture = feature->DeclareCapture(d, plane);
        if (!capture.Color.IsValid()) continue;
        const float blurSigma = PlanarReflectionBlurSigma(plane.Settings.MaxRoughness,
                                                         plane.Settings.ResolutionScale);
        const auto captureColor = BlurCapture(d, capture.Color, blurSigma, index);
        auto params = d.Frame.AllocUpload<Params>();
        if (!params.Valid()) continue;
        std::memcpy(params.Ptr->ReflectionViewProj, capture.Camera.viewProj, sizeof(float) * 16);
        std::memcpy(params.Ptr->WorldToLocal, glm::value_ptr(plane.WorldToLocal), sizeof(float) * 16);
        std::memcpy(params.Ptr->Plane, glm::value_ptr(plane.Equation), sizeof(float) * 4);
        params.Ptr->Settings[0] = 1.0f;
        params.Ptr->Settings[1] = plane.Settings.MaxRoughness;
        params.Ptr->Settings[2] = kPlanarSurfaceTolerance;
        params.Ptr->Settings[3] = kPlanarEdgeFade;
        params.Ptr->Flags[0] = mask.IsValid() ? 1.0f : 0.0f;
        params.Ptr->Flags[1] = captureColor.Id != capture.Color.Id ? blurSigma : 0.0f;
        params.Ptr->Flags[2] = params.Ptr->Flags[3] = 0.0f;
        params.Ptr->Viewport[0] = viewport.X;
        params.Ptr->Viewport[1] = viewport.Y;
        params.Ptr->Viewport[2] = viewport.Width;
        params.Ptr->Viewport[3] = viewport.Height;
        const std::string suffix = "Composite" + std::to_string(index++);
        // Fullscreen consumers resolve their source physical during declaration.
        // Keep the composited HDR pool-backed, like the pipeline's other HDR outputs.
        const auto output = d.Frame.ImportPersistentTexture(d.PassName(suffix.c_str()).c_str(), desc);
        auto maskDesc = desc;
        maskDesc.usage = static_cast<uint32_t>(TextureUsage::ShaderResource | TextureUsage::UnorderedAccess);
        maskDesc.format = static_cast<uint32_t>(TextureFormat::R16_FLOAT);
        const auto nextMask = d.Frame.CreateTexture(d.PassName((suffix + ".Mask").c_str()).c_str(), maskDesc);
        if (!output.IsValid() || !nextMask.IsValid()) continue;
        // A first-pass fallback descriptor is bound but never read (Flags.x=0).
        const auto previousMask = mask.IsValid() ? mask : depth;
        const auto source = input;
        const uint32_t gx = (d.RenderWidth + 7u) / 8u, gy = (d.RenderHeight + 7u) / 8u;
        d.Frame.AddPass(d.PassName(suffix.c_str()).c_str(), PassPhase::kDefault,
            [&](RG::RGPassBuilder& p)
            {
                for (auto texture : {source, capture.Color, captureColor, depth, normal, weight, radiance, previousMask})
                    p.Read(texture, RG::RGTextureRead::SampledCompute);
                p.Write(output, RG::RGTextureWrite::Storage);
                p.Write(nextMask, RG::RGTextureWrite::Storage);
            },
            [=, this](RG::RGContext& ctx)
            {
                auto* device = ctx.GetDevice();
                if (!device || !ctx.Cmd) return;
                const auto pipeline = ctx.GetOrCreatePipelineVariant(m_Pipeline);
                if (!pipeline.IsValid()) return;
                DescriptorSetDesc setDesc{};
                setDesc.layout = m_Layout; setDesc.transient = true;
                setDesc.debugName = "PlanarReflection.Composite.Set0";
                const auto set = device->CreateDescriptorSet(setDesc);
                NamedDescriptorWriter writer(device, set, *m_Meta, 0);
                writer.AddCombinedImageSampler("uScene", ctx.GetTexture(source), m_Sampler);
                writer.AddCombinedImageSampler("uCapture", ctx.GetTexture(captureColor), m_Sampler);
                writer.AddCombinedImageSampler("uSharpCapture", ctx.GetTexture(capture.Color), m_Sampler);
                writer.AddCombinedImageSampler("uDepth", ctx.GetTexture(depth), m_Sampler);
                writer.AddCombinedImageSampler("uNormalRoughness", ctx.GetTexture(normal), m_Sampler);
                writer.AddCombinedImageSampler("uSpecularWeight", ctx.GetTexture(weight), m_Sampler);
                writer.AddCombinedImageSampler("uSpecularRadiance", ctx.GetTexture(radiance), m_Sampler);
                writer.AddCombinedImageSampler("uPreviousMask", ctx.GetTexture(previousMask), m_Sampler);
                writer.AddUniformBuffer("ViewParams", viewParams.Buffer, viewParams.Offset, viewParams.Size);
                writer.AddUniformBuffer("PlanarParams", params.Buffer, params.Offset, sizeof(Params));
                writer.Flush();
                Detail::BindStorageImageByName(device, set, *m_Meta, "uOutput", ctx.GetTexture(output));
                Detail::BindStorageImageByName(device, set, *m_Meta, "uMask", ctx.GetTexture(nextMask));
                ctx.Cmd->SetPipeline(pipeline);
                ctx.Cmd->BindDescriptorSet(0, set, pipeline);
                ctx.Cmd->Dispatch(gx, gy, 1);
            });
        input = output; mask = nextMask;
    }
    d.PublishTexture(m_Output, input);
    if (mask.IsValid()) d.PublishTexture(Names::View::PlanarReflectionMask, mask);
}
}
