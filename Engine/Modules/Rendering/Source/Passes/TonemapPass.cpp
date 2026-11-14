#include "Rendering/Passes/TonemapPass.h"

#include "Rendering/Core/Device.h"
#include "Rendering/Core/NamedDescriptorWriter.h"
#include "Rendering/Core/NamedPushConstantWriter.h"
#include "Rendering/Core/Pipeline/PipelineTypes.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Materials/MaterialHelper.h"
#include "Rendering/Materials/Shader/ShaderMeta.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"
#include "Rendering/Utils/BufferHelpers.h"
#include "Rendering/Utils/CubeLut/CubeLutGpuUpload.h"
#include "Rendering/Utils/TextureUploadHelpers.h"
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
	// Identity LUT tables, bound when the caller supplies no LUT (tonemap.frag declares the
	// bindings whether or not it samples them).
	CubeLutGpuTextures identityLut{};
	bool identityLutCreated = false; // created once per device; a failed creation is logged, not retried
	// 1x1 zero texture bound as the night tint's star light and depth (tonemap.frag declares them); this
	// path pushes tint strength 0, so neither is sampled.
	TextureHandle noNightTintInput{INVALID_TEXTURE_HANDLE};
	// Default exposure-history buffer. tonemap.frag statically reads uExposure even when
	// useAutoExposure==0, so the binding must point at a live buffer; the contents are never
	// consumed on this path (the ternary selects the static push-constant exposure).
	BufferHandle exposureBuffer{INVALID_BUFFER_HANDLE};
	// Zero-filled exposure compensation state (Includes/exposure_compensation.glsl), bound as
	// uCompensation: its valid word is 0, so the night tint's gate reads it as not written.
	BufferHandle noCompensationBuffer{INVALID_BUFFER_HANDLE};
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
static constexpr uint32_t kTonemapCompensationStateBytes = 16u; // Pipeline::kViewExposureCompensationBytes

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
	if (!res.noCompensationBuffer.IsValid())
	{
		BufferDesc compDesc{};
		compDesc.size = kTonemapCompensationStateBytes;
		compDesc.usage = static_cast<uint32_t>(BufferUsage::Storage | BufferUsage::TransferDst);
		compDesc.memoryUsage = BufferMemoryUsage::DeviceLocal;
		compDesc.debugName = "Tonemap.NoCompensation";
		const uint32_t zeros[kTonemapCompensationStateBytes / sizeof(uint32_t)] = {};
		res.noCompensationBuffer = CreateBufferInitialized(device, compDesc, zeros, kTonemapCompensationStateBytes);
	}
	if (!res.identityLutCreated)
	{
		res.identityLutCreated = true;
		res.identityLut = CreateIdentityCubeLutTextures(device);
		if (!res.identityLut.Lut3D.IsValid() || !res.identityLut.Lut1DStrip.IsValid())
			Logger::Log::Error("Passes::Tonemap: the identity Cube LUT tables could not be created; a pass with "
							   "no LUT binds an invalid texture");
	}
	if (!res.noNightTintInput.IsValid())
	{
		TextureDesc td{};
		td.width = 1;
		td.height = 1;
		td.depth = 1;
		td.mipLevels = 1;
		td.arrayLayers = 1;
		td.format = static_cast<uint32_t>(TextureFormat::R16_FLOAT);
		td.usage = static_cast<uint32_t>(TextureUsage::ShaderResource) | static_cast<uint32_t>(TextureUsage::TransferDst);
		td.persistent = true;
		td.debugName = "Tonemap.NoNightTintInput";
		res.noNightTintInput = device->CreateTexture(td);
		if (res.noNightTintInput.IsValid())
		{
			const uint16_t zero = 0;
			UploadTexture2D(device, res.noNightTintInput, &zero, 1, 1, sizeof(zero), "Tonemap.NoNightTintInputStaging");
		}
		else
		{
			Logger::Log::Error("Passes::Tonemap: the night tint's placeholder texture could not be created");
		}
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
	if (res.noCompensationBuffer.IsValid())
	{
		device->DestroyBuffer(res.noCompensationBuffer);
		res.noCompensationBuffer = BufferHandle{};
	}
	if (res.aces2TablesBuffer.IsValid())
	{
		device->DestroyBuffer(res.aces2TablesBuffer);
		res.aces2TablesBuffer = BufferHandle{};
	}
	if (res.noNightTintInput.IsValid())
		device->DestroyTexture(res.noNightTintInput);
	if (res.identityLut.Lut3D.IsValid())
		device->DestroyTexture(res.identityLut.Lut3D);
	if (res.identityLut.Lut1DStrip.IsValid())
		device->DestroyTexture(res.identityLut.Lut1DStrip);
	res.identityLut = {};
	res.identityLutCreated = false;
	g_TonemapPerDevice.erase(it);
}

static bool PrepareTonemapDescriptors(RenderGraph::RGPrepareContext& context,
                                      RenderGraph::RGTexture source,
                                      const DescriptorSetLayoutDesc& layout,
                                      const ShaderMeta& meta,
                                      DescriptorSetHandle& descriptors, CubeLutGpuBindingState lut)
{
    IDevice* device = context.GetDevice();
    if (device == nullptr)
        return false;
    const TextureHandle texture = context.GetTexture(source);
    if (!texture.IsValid())
        return false;
    TonemapDeviceResources* resources = GetOrCreateTonemapResources(device);
    if (!resources || !resources->sampler.IsValid() || !resources->exposureBuffer.IsValid() ||
        !resources->aces2TablesBuffer.IsValid() || !resources->noCompensationBuffer.IsValid() ||
        !resources->noNightTintInput.IsValid() || !resources->identityLut.Lut3D.IsValid() ||
        !resources->identityLut.Lut1DStrip.IsValid())
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
    writer.AddStorageBuffer("uCompensation", resources->noCompensationBuffer, 0, kTonemapCompensationStateBytes);
    writer.AddCombinedImageSampler("uLut3d", lut.Lut3D.IsValid() ? lut.Lut3D : resources->identityLut.Lut3D,
                                   resources->sampler);
    writer.AddCombinedImageSampler("uLut1d", lut.Lut1DStrip.IsValid() ? lut.Lut1DStrip : resources->identityLut.Lut1DStrip,
                                   resources->sampler);
    writer.AddCombinedImageSampler("uStarLight", resources->noNightTintInput, resources->sampler);
    writer.AddCombinedImageSampler("uDepth", resources->noNightTintInput, resources->sampler);
    writer.Flush();
    return true;
}

static void RecordTonemapPass(RenderGraph::RGContext& context, GraphicsPipelineId pipelineId,
                              DescriptorSetHandle descriptors, const NamedPushConstantWriter& pushConstants,
                              uint32_t width, uint32_t rowBegin, uint32_t rowCount)
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
    commands->SetViewport(0.0f, static_cast<float>(rowBegin), static_cast<float>(width), static_cast<float>(rowCount));
    commands->SetScissor(0, static_cast<int>(rowBegin), static_cast<int>(width), static_cast<int>(rowCount));

    commands->BindDescriptorSet(0, descriptors, pipeline);
    commands->Draw(3, 1);
}

static void DeclareTonemapPass(RenderGraph::RGPassBuilder& pass, GraphicsPipelineId pipelineId,
                               DescriptorSetHandle* descriptors, RenderGraph::RGTexture source,
                               RenderGraph::RGTexture destination, const DescriptorSetLayoutDesc& set0,
                               const ShaderMeta& meta, CubeLutGpuBindingState lut, bool partialRows)
{
    pass.UsePipeline(pipelineId);
    pass.Prepare([descriptors, source, set0, meta = &meta, lut](RenderGraph::RGPrepareContext& context) {
        return PrepareTonemapDescriptors(context, source, set0, *meta, *descriptors, lut);
    });
    pass.Read(source, RenderGraph::RGTextureRead::Sampled);
    RenderGraph::RGAttachmentOps ops{};
    // Full-screen triangle overwrites every pixel.
    ops.Load = partialRows ? RenderGraph::RGLoadOp::Load : RenderGraph::RGLoadOp::DontCare;
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
		pushConstants.Add("look", params.look);
		pushConstants.Add("outEncoding", params.outEncoding);
		pushConstants.Add("paperWhiteNits", params.paperWhiteNits);
		pushConstants.Add("maxOutputNits", params.maxOutputNits);
		pushConstants.Add("preserveAlpha", params.preserveAlpha);
        pushConstants.Add("lutIntensity", std::clamp(params.lutIntensity, 0.0f, 1.0f));
        pushConstants.Add("lutInputEncoding", params.lut.InputEncoding);
        pushConstants.Add("lutSize3d", params.lut.Size3D);
        pushConstants.Add("lutSize1d", params.lut.Size1D);
        pushConstants.Add("lutFlags", params.lut.Flags);
        pushConstants.Add("lut1dInMin", params.lut.In1DMin);
        pushConstants.Add("lut1dInMax", params.lut.In1DMax);
        pushConstants.Add("lut3dInMin", params.lut.In3DMin);
        pushConstants.Add("lut3dInMax", params.lut.In3DMax);
	}

	const RenderGraph::RGResourceDesc destinationDesc = frame.Graph().ResourceDesc(destination.Id);
	const uint32_t width = destinationDesc.Width > 0 ? destinationDesc.Width : 1u;
	const uint32_t height = destinationDesc.Height > 0 ? destinationDesc.Height : 1u;
    const bool partialRows = params.targetRowEnd > params.targetRowBegin && params.targetRowEnd <= height &&
                             (params.targetRowBegin > 0 || params.targetRowEnd < height);
    const uint32_t rowBegin = partialRows ? params.targetRowBegin : 0u;
    const uint32_t rowCount = partialRows ? params.targetRowEnd - params.targetRowBegin : height;

	auto* descriptors = frame.AllocatePassData<DescriptorSetHandle>();
	return frame.AddPass(
		passName ? passName : "Tonemap", PassPhase::kFinalize,
		[pipelineId, descriptors, source, destination, &set0, &shaders, lut = params.lut, partialRows](RenderGraph::RGPassBuilder& pass)
		{
		    DeclareTonemapPass(pass, pipelineId, descriptors, source, destination, set0, shaders.meta, lut, partialRows);
		},
		[pipelineId, descriptors, pushConstants, width, rowBegin, rowCount](RenderGraph::RGContext& context)
		{
		    RecordTonemapPass(context, pipelineId, *descriptors, pushConstants, width, rowBegin, rowCount);
		});
}

} // namespace Passes
} // namespace Rendering
} // namespace GameEngine
