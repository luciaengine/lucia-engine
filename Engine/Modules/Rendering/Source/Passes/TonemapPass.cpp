#include "Rendering/Passes/TonemapPass.h"

#include "Rendering/Core/Device.h"
#include "Rendering/Core/NamedDescriptorWriter.h"
#include "Rendering/Core/NamedPushConstantWriter.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Materials/MaterialHelper.h"
#include "Rendering/Materials/ShaderMeta.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"
#include "Rendering/Utils/BufferHelpers.h"
#include "Logger/Logger.h"
#include "TonemapAces2Tables.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace GameEngine {
namespace Rendering {
namespace Passes {

void CleanupTonemapPassForDevice(IDevice* device);

int SelectTonemapOutEncoding(IDevice* device, FinalizeInputSpace inputSpace)
{
	const bool hdrActive = IsHdrOutputModeActive(device->GetActiveHdrOutputMode());

	if (inputSpace == FinalizeInputSpace::EncodedSrgb)
	{
		// No refusal channel exists mid-execute, so the loud channel is an
		// error plus the linear arm: a wrong image with a log line beats a
		// wrong image without one.
		static std::atomic<bool> s_WarnedEncodedInput{false};
		if (!s_WarnedEncodedInput.exchange(true))
		{
			Logger::Log::Error(
				"Tonemap: FinalizeInputSpace::EncodedSrgb is unsupported — tonemap.frag's "
				"chain is defined on scene-linear input only{}. The encode belongs downstream "
				"in the terminal Finalize pass.",
				IsFinalizeSpaceHdrViolation(inputSpace, hdrActive)
					? ", and the encoded arm is SDR-only besides"
					: "");
		}
	}

	if (hdrActive)
		return 4; // HDR linear with headroom; FinalSRGBEncode applies PQ/HLG/scRGB.
	// SDR, unconditionally: display-referred linear, undithered. Whether the ROP
	// or a manual encode applies the OETF changes who quantizes, not who
	// filters — and the pass that quantizes owns the filters, because it is the
	// only one that can see the step.
	return 1;
}


struct TonemapDeviceResources
{
	SamplerHandle sampler{INVALID_SAMPLER_HANDLE};
	// Default exposure-history buffer. tonemap.frag statically reads uExposure even when
	// useAutoExposure==0, so the binding must point at a live buffer; the contents are never
	// consumed on this path (the ternary selects the static push-constant exposure).
	BufferHandle exposureBuffer{INVALID_BUFFER_HANDLE};
	// ACES 2 tier tables (generated TonemapAces2Tables.h payload), uploaded once.
	// Lives in a device buffer, never in shader constants: glslang materializes
	// dynamically indexed const arrays into per-invocation scratch memory, which
	// at this size faults the device.
	BufferHandle aces2TablesBuffer{INVALID_BUFFER_HANDLE};
};

struct TonemapShaderBytes
{
	std::vector<uint8_t> vs;
	std::vector<uint8_t> fs;
	// Reflection meta from the shaderpkg — the single source of truth for the
	// descriptor-set layout and push-constant block. Hand-mirroring the .frag
	// interface here is the drift class that broke the thumbnail tonemap when
	// tonemap.frag gained uExposure (8f8a754a9); layouts and member offsets now
	// come from reflection so a shader edit cannot silently desync this pass.
	ShaderMeta meta;
	bool loaded = false;
	bool warned = false;
};

// Cache tonemap resources per logical device to avoid cross-device handle reuse when
// the Editor creates multiple windows (and therefore multiple IDevice instances).
using DeviceKey = IDevice*;
static std::unordered_map<DeviceKey, TonemapDeviceResources> g_TonemapPerDevice;

// Matches AutoExposureNode's exposure-state buffer ({float scale; uint valid; float pad0,pad1}).
static constexpr uint32_t kTonemapExposureStateBytes = 16u;

static TonemapShaderBytes& GetTonemapShaderBytes(const IDevice& device)
{
	static TonemapShaderBytes s;
	if (!s.loaded)
	{
		ShaderPackage pkg{};
		std::string err;
		if (LoadShaderPkg("Shaders/tonemap.shaderpkg", device.PreferredShaderSource(), pkg, &err))
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
				"Passes::Tonemap: Shaders/tonemap.shaderpkg unavailable ({}); retrying", err);
		}
	}
	return s;
}

static TonemapDeviceResources* GetOrCreateTonemapResources(IDevice* device)
{
	if (!device)
		return nullptr;

	auto [it, inserted] = g_TonemapPerDevice.try_emplace(device);
	if (inserted)
	{
		// Drop this per-device cache whenever the device's GPU objects die —
		// shutdown OR an in-place rebuild after device loss. The cached sampler
		// and exposure buffer belong to one VkDevice; a rebuild destroys them
		// while leaving this IDevice* key alive.
		device->RegisterPerDeviceCacheCleanup(
			"Passes.Tonemap",
			[](IDevice* d)
			{
				CleanupTonemapPassForDevice(d);
			});
	}

	auto& res = it->second;
	if (!res.sampler.IsValid())
	{
		SamplerDesc samplerDesc{};
		samplerDesc.minFilter = 1;
		samplerDesc.magFilter = 1;
		samplerDesc.mipFilter = 1;
		samplerDesc.addressModeU = 2; // clamp
		samplerDesc.addressModeV = 2;
		samplerDesc.addressModeW = 2;
		samplerDesc.debugName = "Tonemap.LinearClamp";
		res.sampler = device->CreateSampler(samplerDesc);
	}
	if (!res.exposureBuffer.IsValid())
	{
		BufferDesc expDesc{};
		expDesc.size = kTonemapExposureStateBytes;
		expDesc.usage = static_cast<uint32_t>(BufferUsage::Storage);
		expDesc.memoryUsage = BufferMemoryUsage::DeviceLocal;
		expDesc.debugName = "Tonemap.DefaultExposure";
		res.exposureBuffer = device->CreateBuffer(expDesc);
	}
	if (!res.aces2TablesBuffer.IsValid())
	{
		BufferDesc tablesDesc{};
		tablesDesc.size = kAces2TablesBytes;
		tablesDesc.usage = static_cast<uint32_t>(BufferUsage::Storage | BufferUsage::TransferDst);
		tablesDesc.memoryUsage = BufferMemoryUsage::DeviceLocal;
		tablesDesc.debugName = "Tonemap.Aces2Tables";
		res.aces2TablesBuffer =
			CreateBufferInitialized(device, tablesDesc, kAces2Tables, kAces2TablesBytes);
	}
	return &res;
}

BufferHandle GetAces2TablesBuffer(IDevice* device)
{
	TonemapDeviceResources* res = GetOrCreateTonemapResources(device);
	return res ? res->aces2TablesBuffer : BufferHandle{};
}

uint32_t GetAces2TablesBufferBytes()
{
	return kAces2TablesBytes;
}

void CleanupTonemapPassForDevice(IDevice* device)
{
	if (!device)
		return;

	auto it = g_TonemapPerDevice.find(device);
	if (it == g_TonemapPerDevice.end())
		return;

	TonemapDeviceResources& res = it->second;
	if (res.sampler.IsValid())
	{
		device->DestroySampler(res.sampler);
		res.sampler = SamplerHandle{};
	}
	if (res.exposureBuffer.IsValid())
	{
		device->DestroyBuffer(res.exposureBuffer);
		res.exposureBuffer = BufferHandle{};
	}
	if (res.aces2TablesBuffer.IsValid())
	{
		device->DestroyBuffer(res.aces2TablesBuffer);
		res.aces2TablesBuffer = BufferHandle{};
	}
	g_TonemapPerDevice.erase(it);
}

static bool PrepareTonemapDescriptors(RenderGraph::RGPrepareContext& context,
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
    TonemapDeviceResources* resources = GetOrCreateTonemapResources(device);
    if (!resources || !resources->sampler.IsValid() || !resources->exposureBuffer.IsValid() ||
        !resources->aces2TablesBuffer.IsValid())
        return false;
    DescriptorSetDesc descriptorDesc{};
    descriptorDesc.layout = layout;
    descriptorDesc.transient = true;
    descriptorDesc.debugName = "Tonemap.DS0";
    descriptors = device->CreateDescriptorSet(descriptorDesc);
    if (!descriptors.IsValid())
        return false;
    NamedDescriptorWriter writer(device, descriptors, meta, 0);
    writer.AddCombinedImageSampler("uHDRColor", texture, resources->sampler);
    writer.AddStorageBuffer("uExposure", resources->exposureBuffer, 0, kTonemapExposureStateBytes);
    writer.AddStorageBuffer("Aces2Tables", resources->aces2TablesBuffer, 0, kAces2TablesBytes);
    writer.Flush();
    return true;
}

static void RecordTonemapPass(RenderGraph::RGContext& context, GraphicsPipelineId pipelineId,
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
    commands->SetViewport(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
    commands->SetScissor(0, 0, static_cast<int>(width), static_cast<int>(height));

    commands->BindDescriptorSet(0, descriptors, pipeline);
    commands->Draw(3, 1);
}

static void DeclareTonemapPass(RenderGraph::RGPassBuilder& pass, GraphicsPipelineId pipelineId,
                               DescriptorSetHandle* descriptors, RenderGraph::RGTexture source,
                               RenderGraph::RGTexture destination, const DescriptorSetLayoutDesc& set0,
                               const ShaderMeta& meta)
{
    pass.UsePipeline(pipelineId);
    pass.Prepare([descriptors, source, set0, meta = &meta](RenderGraph::RGPrepareContext& context) {
        return PrepareTonemapDescriptors(context, source, set0, *meta, *descriptors);
    });
    pass.Read(source, RenderGraph::RGTextureRead::Sampled);
    RenderGraph::RGAttachmentOps ops{};
    // Full-screen triangle overwrites every pixel.
    ops.Load = RenderGraph::RGLoadOp::DontCare;
    ops.Store = RenderGraph::RGStoreOp::Store;
    pass.AttachColor(0, destination, ops);
}

RenderGraph::RGPass AddTonemapPassRG(RenderGraph::RGFrame& frame, RenderGraph::RGTexture source, RenderGraph::RGTexture destination,
                             const TonemapParams& params, const char* passName)
{
	IDevice* device = frame.Device();
	if (!device || !source.IsValid() || !destination.IsValid() || source.Id == destination.Id)
		return {};
	const auto& shaders = GetTonemapShaderBytes(*device);
	if (shaders.vs.empty() || shaders.fs.empty())
		return {}; // loader retries next frame — never latch the failure

	// Layout + push-constant block derived from the shaderpkg's reflection meta
	// (never hand-mirrored — see TonemapShaderBytes). Interned per device; the
	// per-frame cost is the variant-cache hit. Mirrors AddSRGBEncodePassRG.
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
	pipelineDesc.DebugName = "Tonemap.Pipeline";
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
				Logger::Log::Warning("Passes::Tonemap: shader meta rejected ({})", metaError);
			}
			return {};
		}
	}
	const GraphicsPipelineId pipelineId = device->InternGraphicsPipeline(std::move(pipelineDesc));
	if (!pipelineId.IsValid())
		return {};

	// TonemapParams -> push constants by MEMBER NAME against the reflected
	// block, resolved at declaration (the by-value contract). Unnamed members
	// (ditherMode, useAutoExposure) stay zero: outEncoding=1 emits undithered
	// display-referred LINEAR, and this standalone pass never meters.
	NamedPushConstantWriter pushConstants =
		shaders.meta.PushConstants.empty()
			? NamedPushConstantWriter(shaders.meta, std::string{})
			: NamedPushConstantWriter(shaders.meta, shaders.meta.PushConstants[0].Name);
	if (pushConstants.IsValid())
	{
		pushConstants.Add("exposure", params.exposure);
		pushConstants.Add("tonemapMode", params.tonemapMode);
		pushConstants.Add("outEncoding", params.outEncoding);
		pushConstants.Add("paperWhiteNits", params.paperWhiteNits);
		pushConstants.Add("maxOutputNits", params.maxOutputNits);
		pushConstants.Add("ictcpChromaCompression", std::clamp(params.ictcpChromaCompression, 0.0f, 1.0f));
		pushConstants.Add("preserveAlpha", params.preserveAlpha);
	}

	const RenderGraph::RGResourceDesc destinationDesc = frame.Graph().ResourceDesc(destination.Id);
	const uint32_t width = destinationDesc.Width > 0 ? destinationDesc.Width : 1u;
	const uint32_t height = destinationDesc.Height > 0 ? destinationDesc.Height : 1u;

	auto* descriptors = frame.AllocatePassData<DescriptorSetHandle>();
	return frame.AddPass(
		passName ? passName : "Tonemap", PassPhase::kFinalize,
		[pipelineId, descriptors, source, destination, &set0, &shaders](RenderGraph::RGPassBuilder& pass)
		{
		    DeclareTonemapPass(pass, pipelineId, descriptors, source, destination, set0, shaders.meta);
		},
		[pipelineId, descriptors, pushConstants, width, height](RenderGraph::RGContext& context)
		{
		    RecordTonemapPass(context, pipelineId, *descriptors, pushConstants, width, height);
		});
}

} // namespace Passes
} // namespace Rendering
} // namespace GameEngine
