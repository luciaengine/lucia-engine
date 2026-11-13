#include "Rendering/Passes/PixelPerfectUpscalePass.h"

#include "Rendering/Core/Device.h"
#include "Rendering/Core/NamedDescriptorWriter.h"
#include "Rendering/Core/NamedPushConstantWriter.h"
#include "Rendering/Core/Pipeline/PipelineTypes.h"
#include "Rendering/Materials/MaterialHelper.h"
#include "Rendering/Materials/Shader/ShaderMeta.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"
#include "Logger/Logger.h"
#include "Mathematics/Vector2.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace GameEngine {
namespace Rendering {
namespace Passes {

namespace
{
// pixelperfect_upscale.frag's UpscalePC members, written by NAME against the
// reflected block (never hand-mirrored — see TonemapPass for the drift-class
// rationale).
struct UpscaleValues
{
    Mathematics::Vector2 sourceSize{0.0f, 0.0f};
    Mathematics::Vector2 referenceSize{0.0f, 0.0f};
    Mathematics::Vector2 sampleOrigin{0.0f, 0.0f};
    Mathematics::Vector2 outputOrigin{0.0f, 0.0f};
    Mathematics::Vector2 outputSize{0.0f, 0.0f};
    float zoom = 1.0f;
    int32_t outEncoding = 1;
    float paperWhiteNits = 203.0f;
};

struct UpscaleDeviceResources
{
    SamplerHandle sampler{INVALID_SAMPLER_HANDLE};
};

struct UpscaleShaderBytes
{
    std::vector<uint8_t> vs;
    std::vector<uint8_t> fs;
    // Reflection meta — the single source of truth for the descriptor-set
    // layout and push-constant block.
    ShaderMeta meta;
    bool loaded = false;
    bool warned = false;
};

static std::unordered_map<IDevice*, UpscaleDeviceResources> g_UpscalePerDevice;

UpscaleShaderBytes& GetUpscaleShaderBytes(const IDevice& device)
{
    static UpscaleShaderBytes s;
    if (!s.loaded)
    {
        ShaderPackage pkg{};
        std::string err;
        if (LoadShaderPkg("Shaders/pixelperfect_upscale.shaderpkg", device.PreferredShaderSource(), pkg, &err))
        {
            auto itVs = pkg.stageBytes.find("vs");
            auto itFs = pkg.stageBytes.find("fs");
            if (itVs != pkg.stageBytes.end())
                s.vs = std::move(itVs->second);
            if (itFs != pkg.stageBytes.end())
                s.fs = std::move(itFs->second);
            s.meta = std::move(pkg.meta);
        }
        // Latch only on success — a not-yet-ready loader environment retries
        // instead of permanently disabling the pass; the warning fires once.
        s.loaded = !s.vs.empty() && !s.fs.empty();
        if (!s.loaded && !s.warned)
        {
            s.warned = true;
            Logger::Log::Warning(
                "PixelPerfectUpscale: Shaders/pixelperfect_upscale.shaderpkg unavailable ({}); "
                "retrying",
                err);
        }
    }
    return s;
}

SamplerHandle GetOrCreateUpscaleSampler(IDevice* device)
{
    if (!device)
        return {};

    auto [it, inserted] = g_UpscalePerDevice.try_emplace(device);
    if (inserted)
    {
        // Drop this per-device cache whenever the device's GPU objects die —
        // shutdown OR an in-place rebuild after device loss. The cached sampler
        // belongs to one VkDevice; a rebuild destroys it while leaving this
        // IDevice* key alive.
        device->RegisterPerDeviceCacheCleanup(
            "Passes.PixelPerfectUpscale",
            [](IDevice* d) { CleanupPixelPerfectUpscalePassForDevice(d); });
    }

    auto& res = it->second;
    if (res.sampler.IsValid())
        return res.sampler;

    // Nearest (point) filtering with clamp: crisp blocks, no bleeding at the
    // padded RT border when the sub-pixel window shift reaches its extreme.
    SamplerDesc samplerDesc{};
    samplerDesc.minFilter = 0; // nearest
    samplerDesc.magFilter = 0; // nearest
    samplerDesc.mipFilter = 0;
    samplerDesc.addressModeU = 2; // clamp to edge
    samplerDesc.addressModeV = 2;
    samplerDesc.addressModeW = 2;
    samplerDesc.debugName = "PixelPerfectUpscale.NearestClamp";
    res.sampler = device->CreateSampler(samplerDesc);
    return res.sampler;
}

int32_t SelectOutputEncoding(IDevice* device, bool useBackbuffer)
{
    // HDR transfer functions are display-referred — they only belong on the
    // swapchain. An offscreen target gets plain sRGB regardless of the display's
    // active HDR mode, so a readback of it yields a correct SDR image.
    const HdrOutputMode outputMode = useBackbuffer ? device->GetActiveHdrOutputMode() : HdrOutputMode::Off;
    switch (outputMode)
    {
    case HdrOutputMode::HDR10_PQ:
    case HdrOutputMode::HDR10Plus:
        return 2;
    case HdrOutputMode::HLG:
        return 3;
    case HdrOutputMode::ScRGB:
        return 4;
    default:
        // SDR. A UNORM swapchain needs the manual linear->sRGB encode; an sRGB
        // swapchain hardware-encodes on write, so pass linear through (encoding 4
        // with paperWhite 80 -> scale 1.0). Dedicated offscreen targets always
        // need the manual encode.
        return (!useBackbuffer || device->SwapchainNeedsManualSRGBEncode()) ? 1 : 4;
    }
}

} // namespace

void CleanupPixelPerfectUpscalePassForDevice(IDevice* device)
{
    if (!device)
        return;
    auto it = g_UpscalePerDevice.find(device);
    if (it == g_UpscalePerDevice.end())
        return;
    if (it->second.sampler.IsValid())
        device->DestroySampler(it->second.sampler);
    g_UpscalePerDevice.erase(it);
}

static bool PreparePixelPerfectUpscaleDescriptors(RenderGraph::RGPrepareContext& context,
                                      RenderGraph::RGTexture source,
                                      const DescriptorSetLayoutDesc& layout,
                                      const ShaderMeta& meta,
                                      DescriptorSetHandle& descriptors)
{
    IDevice* device = context.GetDevice();
    if (device == nullptr)
        return false;
    const TextureHandle texture = context.GetTexture(source);
    if (!texture.IsValid())
        return false;
    const SamplerHandle sampler = GetOrCreateUpscaleSampler(device);
    if (!sampler.IsValid())
        return false;
    DescriptorSetDesc descriptorDesc{};
    descriptorDesc.layout = layout;
    descriptorDesc.transient = true;
    descriptorDesc.debugName = "PixelPerfectUpscale.DS0";
    descriptors = device->CreateDescriptorSet(descriptorDesc);
    if (!descriptors.IsValid())
        return false;
    NamedDescriptorWriter writer(device, descriptors, meta, 0);
    writer.AddCombinedImageSampler("uSourceColor", texture, sampler);
    writer.Flush();
    return true;
}

static void RecordPixelPerfectUpscalePass(RenderGraph::RGContext& context, GraphicsPipelineId pipelineId,
                              DescriptorSetHandle descriptors, const NamedPushConstantWriter& pushConstants,
                              uint32_t width, uint32_t height)
{
    auto* commands = context.Cmd;
    auto* device = context.GetDevice();
    if (!commands || !device)
        return;
    const PipelineHandle pipeline = device->TryGetWarmGraphicsPipeline(pipelineId, context.BuildCurrentFormatKey());
    if (!pipeline.IsValid())
        return;

    commands->SetPipeline(pipeline);
    if (pushConstants.IsValid())
        pushConstants.Flush(commands);
    // Viewport is the full destination — the shader letterboxes via
    // gl_FragCoord.
    commands->SetViewport(0.0f, 0.0f, static_cast<float>(width),
                    static_cast<float>(height));
    commands->SetScissor(0, 0, static_cast<int>(width), static_cast<int>(height));

    commands->BindDescriptorSet(0, descriptors, pipeline);
    commands->Draw(3, 1);
}

static void DeclarePixelPerfectUpscalePass(RenderGraph::RGPassBuilder& pass, GraphicsPipelineId pipelineId,
                               DescriptorSetHandle* descriptors, RenderGraph::RGTexture source,
                               RenderGraph::RGTexture destination, const DescriptorSetLayoutDesc& set0,
                               const ShaderMeta& meta)
{
    pass.UsePipeline(pipelineId);
    pass.Prepare([descriptors, source, set0, meta = &meta](RenderGraph::RGPrepareContext& context) {
        return PreparePixelPerfectUpscaleDescriptors(context, source, set0, *meta, *descriptors);
    });
    pass.Read(source, RenderGraph::RGTextureRead::Sampled);
    RenderGraph::RGAttachmentOps ops{};
    // The shader writes every pixel, including black letterbox bars.
    ops.Load = RenderGraph::RGLoadOp::DontCare;
    ops.Store = RenderGraph::RGStoreOp::Store;
    pass.AttachColor(0, destination, ops);
}

RenderGraph::RGPass AddPixelPerfectUpscalePassRG(RenderGraph::RGFrame& frame,
                                         const PixelPerfectUpscaleParamsRG& params,
                                         FinalizeInputSpace inputSpace,
                                         const char* passName)
{
    IDevice* device = frame.Device();
    if (!device || !params.Src.IsValid() || !params.Dst.IsValid() ||
        params.Src.Id == params.Dst.Id)
        return {};
    // Finalize contract (FinalizeContract.h): an already-encoded input is
    // SDR-only. Refusing to declare is this pass's loud channel — a missing
    // upscale is unmissable, a silently wrong re-encode is not.
    const bool dstIsBackbuffer = frame.IsBackbuffer(params.Dst);
    const HdrOutputMode outputMode =
        dstIsBackbuffer ? device->GetActiveHdrOutputMode() : HdrOutputMode::Off;
    if (IsFinalizeSpaceHdrViolation(inputSpace, IsHdrOutputModeActive(outputMode)))
    {
        static bool s_WarnedEncodedHdr = false;
        if (!s_WarnedEncodedHdr)
        {
            s_WarnedEncodedHdr = true;
            Logger::Log::Error(
                "PixelPerfectUpscale: '{}' declared FinalizeInputSpace::EncodedSrgb under an "
                "active HDR output mode — the encoded arm is SDR-only; pass not declared",
                passName ? passName : "PixelPerfectUpscale");
        }
        return {};
    }
    // I11 needs no check here: this pass samples NEAREST (NearestClamp, below),
    // so it replicates texels rather than mixing them and cannot low-pass the
    // dither an encoded source arrives with — which is exactly the exemption the
    // invariant is carved around. The transport that CAN break it is the terminal
    // encode, which samples linear, and it asserts the extent rule on itself.
    const auto& shaders = GetUpscaleShaderBytes(*device);
    if (shaders.vs.empty() || shaders.fs.empty())
        return {};

    // Layout + push-constant block derived from the shaderpkg's reflection
    // meta. Interned per device; the per-frame cost is the variant-cache hit.
    GraphicsPipelineDesc pipelineDesc{};
    pipelineDesc.Kind = GraphicsPipelineKind::VertexFragment;
    pipelineDesc.VertexShader = std::make_shared<const std::vector<uint8_t>>(shaders.vs);
    pipelineDesc.PixelShader = std::make_shared<const std::vector<uint8_t>>(shaders.fs);
    pipelineDesc.Topology = PrimitiveTopology::TriangleList;
    pipelineDesc.Rasterization.cullMode = CullModeFlagBits::None;
    pipelineDesc.Rasterization.frontFace = FrontFace::CounterClockwise;
    pipelineDesc.DepthStencil.depthTestEnable = false;
    pipelineDesc.DepthStencil.depthWriteEnable = false;
    DynamicStateInfo dynamicState{};
    dynamicState.states.push_back(DynamicState::Viewport);
    dynamicState.states.push_back(DynamicState::Scissor);
    pipelineDesc.DynamicState = std::move(dynamicState);
    pipelineDesc.DebugName = "PixelPerfectUpscale.Pipeline";
    DescriptorSetLayoutDesc set0{};
    {
        std::string metaError;
        if (!MaterialHelper::ApplyShaderMetaToGraphicsDesc(
                *device, shaders.meta, pipelineDesc, MaterialBuilder::MergeMode::Auto, {true, 128},
                [&set0](uint32_t setIndex, DescriptorSetLayoutDesc& setLayout)
                {
                    if (setIndex == 0)
                        set0 = setLayout;
                },
                &metaError))
        {
            static bool s_WarnedMeta = false;
            if (!s_WarnedMeta)
            {
                s_WarnedMeta = true;
                Logger::Log::Warning("PixelPerfectUpscale: shader meta rejected ({})", metaError);
            }
            return {};
        }
    }
    const GraphicsPipelineId pipelineId = device->InternGraphicsPipeline(std::move(pipelineDesc));
    if (!pipelineId.IsValid())
        return {};

    // Push constants computed at DECLARATION (the by-value contract): the
    // destination extent comes from the frame's resource desc, the letterbox
    // math mirrors the old pass's Execute, and the output encoding follows
    // the declared input space — a linear input keeps the passthrough /
    // self-encode duality; an encoded input passes bytes through raw (or
    // hands an _SRGB destination D(c)).
    const RenderGraph::RGResourceDesc destinationDesc = frame.Graph().ResourceDesc(params.Dst.Id);
    const uint32_t width = destinationDesc.Width > 0 ? destinationDesc.Width : 1u;
    const uint32_t height = destinationDesc.Height > 0 ? destinationDesc.Height : 1u;

    const float zoom = static_cast<float>(std::max(1u, params.Zoom));
    const float refW = static_cast<float>(std::max(1u, params.ReferenceWidth));
    const float refH = static_cast<float>(std::max(1u, params.ReferenceHeight));
    const float outW = refW * zoom;
    const float outH = refH * zoom;
    // Center the upscaled rect; floor so the origin lands on a whole pixel.
    const float outOriginX = std::floor((static_cast<float>(width) - outW) * 0.5f);
    const float outOriginY = std::floor((static_cast<float>(height) - outH) * 0.5f);

    UpscaleValues pc{};
    pc.sourceSize[0] = static_cast<float>(std::max(1u, params.SourceWidth));
    pc.sourceSize[1] = static_cast<float>(std::max(1u, params.SourceHeight));
    pc.referenceSize[0] = refW;
    pc.referenceSize[1] = refH;
    // Sample-window origin = the one-texel border, moved by the sub-pixel
    // remainder in the direction the camera moved. Texel columns grow with
    // world X, so a camera to the right of its snap reads further right; texel
    // rows grow downward against world Y, so a camera above its snap reads
    // further up.
    pc.sampleOrigin[0] = 1.0f + params.FracX;
    pc.sampleOrigin[1] = 1.0f - params.FracY;
    pc.outputOrigin[0] = outOriginX;
    pc.outputOrigin[1] = outOriginY;
    pc.outputSize[0] = outW;
    pc.outputSize[1] = outH;
    pc.zoom = zoom;
    if (inputSpace == FinalizeInputSpace::EncodedSrgb)
    {
        // Encoded bytes pass through untouched — the point-sampled integer
        // upscale is byte-preserving and the OETF already ran upstream. The
        // only transform is destination-keyed, mirroring encode_srgb's arm 6:
        // an _SRGB destination takes D(c) so the ROP's re-encode round-trips
        // byte-exact; everything else takes the raw bytes (0).
        const TextureFormat dstFormat =
            dstIsBackbuffer
                ? device->GetSwapchainTextureFormat()
                : static_cast<TextureFormat>(frame.Graph().ResourceDesc(params.Dst.Id).Format);
        const bool dstIsSrgbRop =
            dstFormat == TextureFormat::RGBA8_SRGB || dstFormat == TextureFormat::BGRA8_SRGB;
        pc.outEncoding = dstIsSrgbRop ? 6 : 0;
    }
    else
    {
        pc.outEncoding =
            params.PassthroughLinear ? 0 : SelectOutputEncoding(device, dstIsBackbuffer);
    }
    pc.paperWhiteNits = device->GetHdrOutputState().staticMetadata.paperWhiteNits;

    // Values -> push constants by MEMBER NAME against the reflected block,
    // resolved at declaration (the by-value contract).
    NamedPushConstantWriter pushConstants =
        shaders.meta.PushConstants.empty()
            ? NamedPushConstantWriter(shaders.meta, std::string{})
            : NamedPushConstantWriter(shaders.meta, shaders.meta.PushConstants[0].Name);
    if (pushConstants.IsValid())
    {
        pushConstants.Add("sourceSize", pc.sourceSize);
        pushConstants.Add("referenceSize", pc.referenceSize);
        pushConstants.Add("sampleOrigin", pc.sampleOrigin);
        pushConstants.Add("outputOrigin", pc.outputOrigin);
        pushConstants.Add("outputSize", pc.outputSize);
        pushConstants.Add("zoom", pc.zoom);
        pushConstants.Add("outEncoding", pc.outEncoding);
        pushConstants.Add("paperWhiteNits", pc.paperWhiteNits);
    }

    const RenderGraph::RGTexture source = params.Src;
    auto* descriptors = frame.AllocatePassData<DescriptorSetHandle>();
    return frame.AddPass(
        passName ? passName : "PixelPerfectUpscale", PassPhase::kFinalize,
        [pipelineId, descriptors, source, &set0, &shaders, &params](RenderGraph::RGPassBuilder& pass)
        {
            DeclarePixelPerfectUpscalePass(pass, pipelineId, descriptors, source, params.Dst, set0, shaders.meta);
        },
        [pipelineId, descriptors, pushConstants, width, height](RenderGraph::RGContext& context)
        {
            RecordPixelPerfectUpscalePass(context, pipelineId, *descriptors, pushConstants, width, height);
        });
}

} // namespace Passes
} // namespace Rendering
} // namespace GameEngine
