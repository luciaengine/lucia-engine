#include "Rendering/Core/RenderGraph/RGFullscreen.h"

#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Pipeline/PipelineTypes.h"
#include "Rendering/Materials/Shader/ShaderLayoutShape.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <array>
#include <cassert>

namespace GameEngine::Rendering::RenderGraph
{

PipelineDesc MakeFullscreenPipelineDesc(const char* debugName)
{
    PipelineDesc pd{};
    pd.type = PipelineType::Graphics;
    pd.rasterizationSamples = 1;
    pd.EnableDepthTest(false);
    pd.SetCullingMode(CullModeFlagBits::None);
    pd.AddDynamicState(DynamicState::Viewport);
    pd.AddDynamicState(DynamicState::Scissor);
    pd.debugName = debugName;
    return pd;
}

namespace
{
struct FullscreenPassData
{
    std::array<RGFullscreenTextureInput, kMaxFullscreenTextureInputs> Textures{};
    std::array<RGFullscreenBufferInput, kMaxFullscreenBufferInputs> Buffers{};
    uint32_t TextureCount = 0;
    uint32_t BufferCount = 0;
    uint32_t Width = 0;
    uint32_t Height = 0;
    GraphicsPipelineId PipelineId{};
    DescriptorSetLayoutDesc Layout;
    PipelineHandle Pipeline{};
    DescriptorSetHandle DescriptorSet{};
};

bool PrepareFullscreenPass(RGPrepareContext& context, FullscreenPassData& data)
{
    IDevice* device = context.GetDevice();
    if (device == nullptr)
        return false;
    data.Pipeline = context.PreparePipeline(data.PipelineId);
    if (!data.Pipeline.IsValid())
        return false;

    std::array<TextureHandle, kMaxFullscreenTextureInputs> resolved{};
    std::array<SamplerHandle, kMaxFullscreenTextureInputs> samplers{};
    for (uint32_t index = 0; index < data.TextureCount; ++index)
    {
        const RGFullscreenTextureInput& input = data.Textures[index];
        TextureHandle physical{};
        if (input.Texture.IsValid())
        {
            physical = context.GetTexture(input.Texture);
            if (!physical.IsValid() && input.Required)
                return false;
        }
        if (!physical.IsValid())
            physical = input.RawTexture;
        SamplerHandle sampler = input.Sampler;
        if (!physical.IsValid())
        {
            physical = input.Fallback;
            if (input.FallbackSampler.IsValid())
                sampler = input.FallbackSampler;
        }
        if (!physical.IsValid() || !sampler.IsValid())
            return false;
        resolved[index] = physical;
        samplers[index] = sampler;
    }
    for (uint32_t index = 0; index < data.BufferCount; ++index)
        if (!data.Buffers[index].Buffer.IsValid())
            return false;

    DescriptorSetDesc descriptorDesc{};
    descriptorDesc.layout = data.Layout;
    descriptorDesc.transient = true;
    descriptorDesc.debugName = "RGFullscreen.DS";
    data.DescriptorSet = device->CreateDescriptorSet(descriptorDesc);
    if (!data.DescriptorSet.IsValid())
        return false;
    for (uint32_t index = 0; index < data.TextureCount; ++index)
        device->UpdateCombinedImageSamplerBinding(data.DescriptorSet, data.Textures[index].Binding,
                                                 resolved[index], samplers[index]);
    for (uint32_t index = 0; index < data.BufferCount; ++index)
    {
        const RGFullscreenBufferInput& buffer = data.Buffers[index];
        if (buffer.IsStorage)
            device->UpdateStorageBufferBinding(data.DescriptorSet, buffer.Binding, buffer.Buffer,
                                                static_cast<size_t>(buffer.Offset),
                                                static_cast<size_t>(buffer.Size));
        else
            device->UpdateBufferBinding(data.DescriptorSet, buffer.Binding, buffer.Buffer,
                                         static_cast<size_t>(buffer.Offset),
                                         static_cast<size_t>(buffer.Size));
    }
    return true;
}

void RecordFullscreenPass(RGContext& context, const FullscreenPassData& data)
{
    CommandList* commands = context.Cmd;
    if (commands == nullptr)
        return;
    commands->SetPipeline(data.Pipeline);
    commands->SetViewport(0.0f, 0.0f, static_cast<float>(data.Width), static_cast<float>(data.Height));
    commands->SetScissor(0, 0, data.Width, data.Height);
    commands->BindDescriptorSet(0, data.DescriptorSet, data.Pipeline);
    commands->Draw(3, 1);
}

void DeclareFullscreenPass(RGPassBuilder& pass, FullscreenPassData& data,
                           RGTexture target, RGTexture secondTarget, const RGAttachmentOps& attachmentOps)
{
    for (uint32_t index = 0; index < data.TextureCount; ++index)
        if (data.Textures[index].Texture.IsValid())
            pass.Read(data.Textures[index].Texture, RGTextureRead::Sampled);
    for (uint32_t index = 0; index < data.BufferCount; ++index)
    {
        const RGFullscreenBufferInput& buffer = data.Buffers[index];
        if (buffer.Declare.IsValid())
            pass.Read(buffer.Declare, buffer.IsStorage ? RGBufferRead::Storage : RGBufferRead::Uniform);
    }
    pass.AttachColor(0, target, attachmentOps);
    if (secondTarget.IsValid())
        pass.AttachColor(1, secondTarget, attachmentOps);
    pass.Prepare([data = &data](RGPrepareContext& context) { return PrepareFullscreenPass(context, *data); });
}
} // namespace

RGPass AddFullscreenPass(RGFrame& frame, const RGFullscreenDesc& desc)
{
    assert(desc.Name && desc.Target.IsValid() && "AddFullscreenPass: name + target required");
    if (desc.Textures.size() > kMaxFullscreenTextureInputs ||
        desc.Buffers.size() > kMaxFullscreenBufferInputs)
    {
        Logger::Log::Error("AddFullscreenPass '{}': {} textures and {} buffers exceed the limits of {} textures "
                           "and {} buffers; no pass was declared",
                           desc.Name, desc.Textures.size(), desc.Buffers.size(), kMaxFullscreenTextureInputs,
                           kMaxFullscreenBufferInputs);
        return {};
    }

    const RGResourceDesc& targetDesc = frame.Graph().ResourceDesc(desc.Target.Id);
    if (desc.SecondTarget.IsValid())
    {
        const RGResourceDesc& second = frame.Graph().ResourceDesc(desc.SecondTarget.Id);
        if (second.Width != targetDesc.Width || second.Height != targetDesc.Height ||
            second.SampleCount != targetDesc.SampleCount)
        {
            Logger::Log::Error("AddFullscreenPass '{}': the second target is {}x{} x{} and the target is {}x{} x{}; "
                               "give both the same size and sample count. The pass is not declared",
                               desc.Name, second.Width, second.Height, second.SampleCount, targetDesc.Width,
                               targetDesc.Height, targetDesc.SampleCount);
            return {};
        }
    }
    const uint32_t width = desc.Width == 0 ? targetDesc.Width : desc.Width;
    const uint32_t height = desc.Height == 0 ? targetDesc.Height : desc.Height;
    RGAttachmentOps attachmentOps = desc.Ops;
    if (attachmentOps.WritesEveryPixel && (width != targetDesc.Width || height != targetDesc.Height))
    {
        assert(false && "AddFullscreenPass: WritesEveryPixel needs a viewport that covers the target");
        attachmentOps.WritesEveryPixel = false;
    }

    FullscreenPassData* data = frame.AllocatePassData<FullscreenPassData>();
    data->TextureCount = static_cast<uint32_t>(desc.Textures.size());
    data->BufferCount = static_cast<uint32_t>(desc.Buffers.size());
    std::copy_n(desc.Textures.begin(), data->TextureCount, data->Textures.begin());
    std::copy_n(desc.Buffers.begin(), data->BufferCount, data->Buffers.begin());
    data->PipelineId = desc.Pipeline;
    data->Layout = desc.Layout;
    data->Width = width;
    data->Height = height;

    return frame.AddPass(
        desc.Name, desc.Phase,
        [data, &desc, attachmentOps](RGPassBuilder& pass)
        { DeclareFullscreenPass(pass, *data, desc.Target, desc.SecondTarget, attachmentOps); },
        [data](RGContext& context) { RecordFullscreenPass(context, *data); });
}

RGPass AddCopyPass(RGFrame& frame, RGTexture source, RGTexture dest, const char* name,
                   int32_t phase, GraphicsPipelineId pipeline, GraphicsPipelineId msaaPipeline,
                   const DescriptorSetLayoutDesc& layout, const DescriptorSetLayoutDesc& msaaLayout,
                   SamplerHandle sampler, uint32_t width, uint32_t height)
{
    const RGResourceDesc& sd = frame.Graph().ResourceDesc(source.Id);
    const bool msaa = sd.SampleCount > 1;

    RGFullscreenTextureInput input{};
    input.Binding = 0;
    input.Texture = source;
    input.Sampler = sampler;
    input.Required = true;

    RGFullscreenDesc desc{};
    desc.Name = name;
    desc.Phase = phase;
    desc.Pipeline = msaa ? msaaPipeline : pipeline;
    desc.Layout = msaa ? msaaLayout : layout;
    desc.Textures = std::span<const RGFullscreenTextureInput>(&input, 1);
    desc.Target = dest;
    desc.Ops.Load = RGLoadOp::DontCare;
    desc.Ops.Store = RGStoreOp::Store;
    desc.Width = width;
    desc.Height = height;
    return AddFullscreenPass(frame, desc);
}

bool LoadCopyPipelineDesc(ShaderSourceKind kind, const char* pipelineName, const char* layoutName,
                          DescriptorSetLayoutDesc& outLayout, PipelineDesc& outPipeline)
{
    ShaderPackage pkg{};
    if (!LoadShaderPkg("Shaders/copy.shaderpkg", kind, pkg))
        return false;
    const auto vs = pkg.stageBytes.find("vs");
    const auto fs = pkg.stageBytes.find("fs");
    if (vs == pkg.stageBytes.end() || fs == pkg.stageBytes.end() || vs->second.empty() ||
        fs->second.empty())
        return false;

    outLayout = {};
    outLayout.debugName = layoutName;
    outLayout.bindings = {{0, DescriptorType::CombinedImageSampler, 1, kShaderStageFragment}};
    ApplyMetaImageShapeToLayout(pkg.meta, outLayout);

    outPipeline = MakeFullscreenPipelineDesc(pipelineName);
    outPipeline.vertexShader = vs->second;
    outPipeline.pixelShader = fs->second;
    outPipeline.descriptorSetLayouts.push_back(outLayout);
    return true;
}

} // namespace GameEngine::Rendering::RenderGraph
