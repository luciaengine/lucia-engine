#include "Rendering/Core/RenderGraph/RGFullscreen.h"

#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Materials/ShaderLayoutShape.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"

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
} // namespace

RGPass AddFullscreenPass(RGFrame& frame, const RGFullscreenDesc& desc)
{
    assert(desc.Name && desc.Target.IsValid() && "AddFullscreenPass: name + target required");
    assert(desc.Textures.size() <= kMaxFullscreenTextureInputs);
    assert(desc.Buffers.size() <= kMaxFullscreenBufferInputs);

    FullscreenPassData* data = frame.AllocatePassData<FullscreenPassData>();
    data->TextureCount = static_cast<uint32_t>(std::min(desc.Textures.size(), data->Textures.size()));
    data->BufferCount = static_cast<uint32_t>(std::min(desc.Buffers.size(), data->Buffers.size()));
    std::copy_n(desc.Textures.begin(), data->TextureCount, data->Textures.begin());
    std::copy_n(desc.Buffers.begin(), data->BufferCount, data->Buffers.begin());
    data->PipelineId = desc.Pipeline;
    data->Layout = desc.Layout;
    const RGResourceDesc& targetDesc = frame.Graph().ResourceDesc(desc.Target.Id);
    data->Width = desc.Width == 0 ? targetDesc.Width : desc.Width;
    data->Height = desc.Height == 0 ? targetDesc.Height : desc.Height;

    return frame.AddPass(
        desc.Name, desc.Phase,
        [&](RGPassBuilder& pass)
        {
            for (uint32_t index = 0; index < data->TextureCount; ++index)
                if (data->Textures[index].Texture.IsValid())
                    pass.Read(data->Textures[index].Texture, RGTextureRead::Sampled);
            for (uint32_t index = 0; index < data->BufferCount; ++index)
            {
                const RGFullscreenBufferInput& buffer = data->Buffers[index];
                if (buffer.Declare.IsValid())
                    pass.Read(buffer.Declare, buffer.IsStorage ? RGBufferRead::Storage : RGBufferRead::Uniform);
            }
            pass.AttachColor(0, desc.Target, desc.Ops);
            pass.Prepare([data](RGPrepareContext& context) { return PrepareFullscreenPass(context, *data); });
        },
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
