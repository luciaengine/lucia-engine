#include "Rendering/Passes/MsaaColorResolvePass.h"

#include "Rendering/Core/Device.h"
#include "Rendering/Core/PassPhase.h"
#include "Rendering/Core/Pipeline/PipelineDescTranslator.h"
#include "Rendering/Core/RenderGraph/RGFullscreen.h"
#include "Rendering/Materials/Shader/ShaderLayoutShape.h"
#include "Rendering/Passes/TonemapPass.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"
#include "Logger/Logger.h"
#include "Types/FlatMap.h"

#include <array>
#include <atomic>
#include <mutex>
#include <string>
#include <utility>

namespace GameEngine::Rendering::Passes
{
namespace
{
constexpr const char* kPackagePath = "Shaders/msaa_color_resolve.shaderpkg";
constexpr const char* kBloomDeltaPackagePath = "Shaders/msaa_color_resolve_bloom_delta.shaderpkg";
// tonemap_aces2_tables.glsl, view_params.glsl and view_exposure.glsl.
constexpr uint32_t kAces2TablesBinding = 2;
constexpr uint32_t kViewParamsBinding = 7;
constexpr uint32_t kExposureHistoryBinding = 47;

// One device's resolve program. The pipeline id names an interned description and survives a
// device rebuild; the sampler is a GPU object, so the per-device cleanup drops the entry.
struct ResolveDeviceResources
{
    GraphicsPipelineId Pipeline{};
    // The same resolve with a second output, the bloom sample delta.
    GraphicsPipelineId BloomDeltaPipeline{};
    DescriptorSetLayoutDesc Layout{};
    SamplerHandle Sampler{};
    // TonemapPass owns it; its per-device cleanup frees it when this entry drops.
    BufferHandle Aces2Tables{};
};

std::mutex g_ResolveMutex;
FlatMap<IDevice*, ResolveDeviceResources> g_ResolvePerDevice;

void DropResolveResources(IDevice* device)
{
    std::lock_guard lock(g_ResolveMutex);
    ResolveDeviceResources* resources = g_ResolvePerDevice.Find(device);
    if (!resources)
        return;
    if (resources->Sampler.IsValid())
        device->DestroySampler(resources->Sampler);
    g_ResolvePerDevice.Erase(device);
}

GraphicsPipelineId LoadResolvePipeline(IDevice& device, const char* packagePath, const char* debugName,
                                       DescriptorSetLayoutDesc& layout)
{
    ShaderPackage package{};
    std::string error;
    if (!LoadShaderPkg(packagePath, device.PreferredShaderSource(), package, &error))
    {
        static std::atomic<bool> s_Reported{false};
        if (!s_Reported.exchange(true))
            Logger::Log::Error("MsaaColorResolvePass: {} did not load ({}); multisampled scene color is not "
                               "resolved. Build the CompileShaderPkgs target to stage it.",
                               packagePath, error);
        return {};
    }
    const auto vertex = package.stageBytes.find("vs");
    const auto fragment = package.stageBytes.find("fs");
    if (vertex == package.stageBytes.end() || fragment == package.stageBytes.end())
    {
        Logger::Log::Error("MsaaColorResolvePass: {} has no vs/fs stage", packagePath);
        return {};
    }

    layout.debugName = "MsaaColorResolve.Set0";
    layout.bindings = {{0, DescriptorType::CombinedImageSampler, 1, kShaderStageFragment},
                       {kAces2TablesBinding, DescriptorType::StorageBuffer, 1, kShaderStageFragment},
                       {kViewParamsBinding, DescriptorType::UniformBuffer, 1, kShaderStageFragment},
                       {kExposureHistoryBinding, DescriptorType::StorageBuffer, 1, kShaderStageFragment}};
    ApplyMetaImageShapeToLayout(package.meta, layout);
    PipelineDesc pipeline = RenderGraph::MakeFullscreenPipelineDesc(debugName);
    pipeline.vertexShader = vertex->second;
    pipeline.pixelShader = fragment->second;
    pipeline.descriptorSetLayouts.push_back(layout);
    return PipelineDescTranslator::InternGraphics(device, pipeline);
}

bool LoadResolveResources(IDevice& device, ResolveDeviceResources& out)
{
    // Both packages reflect one sampler2DMS at binding 0, the ACES 2 tables and the two view buffers, so one layout
    // serves both.
    out.Pipeline = LoadResolvePipeline(device, kPackagePath, "MsaaColorResolve", out.Layout);
    out.BloomDeltaPipeline =
        LoadResolvePipeline(device, kBloomDeltaPackagePath, "MsaaColorResolveBloomDelta", out.Layout);
    if (!out.Pipeline.IsValid() || !out.BloomDeltaPipeline.IsValid())
        return false;
    // texelFetch ignores the filter; the binding is a combined image sampler and needs one.
    out.Sampler = device.CreateSampler(SamplerDesc::PointClamp("MsaaColorResolve.PointClamp"));
    out.Aces2Tables = GetAces2TablesBuffer(&device);
    if (!out.Aces2Tables.IsValid())
        Logger::Log::Error("MsaaColorResolvePass: the ACES 2 tables buffer could not be created; multisampled scene "
                           "color is not resolved");
    return out.Sampler.IsValid() && out.Aces2Tables.IsValid();
}

// Copies this device's resources into `out`, loading them on first use. A failed load is
// retried on the next declaration, so staging the package mid-session recovers.
bool EnsureResolveResources(IDevice& device, ResolveDeviceResources& out)
{
    std::lock_guard lock(g_ResolveMutex);
    if (const ResolveDeviceResources* cached = g_ResolvePerDevice.Find(&device))
    {
        out = *cached;
        return true;
    }
    ResolveDeviceResources resources{};
    if (!LoadResolveResources(device, resources))
    {
        if (resources.Sampler.IsValid())
            device.DestroySampler(resources.Sampler);
        return false;
    }
    device.RegisterPerDeviceCacheCleanup("Passes.MsaaColorResolve", DropResolveResources);
    out = g_ResolvePerDevice.InsertOrAssign(&device, std::move(resources));
    return true;
}

} // namespace

RenderGraph::RGPass AddMsaaColorResolvePassRG(RenderGraph::RGFrame& frame, RenderGraph::RGTexture multisampled,
                                              RenderGraph::RGTexture resolved, RenderGraph::RGTexture bloomSampleDelta,
                                              const MsaaResolveViewBuffers& view, const char* passName)
{
    IDevice* device = frame.Device();
    if (!device || !multisampled.IsValid() || !resolved.IsValid())
        return {};
    if (!view.ViewParams.Buffer.IsValid() || !view.ExposureHistory.Buffer.IsValid())
    {
        Logger::Log::Error("MsaaColorResolvePass '{}': the view's ViewParams or ExposureHistory buffer is invalid; "
                           "the pass is not declared. Pass the view's buffers or the engine's fallbacks.",
                           passName);
        return {};
    }
    const RenderGraph::RGResourceDesc& source = frame.Graph().ResourceDesc(multisampled.Id);
    const RenderGraph::RGResourceDesc& target = frame.Graph().ResourceDesc(resolved.Id);
    if (source.SampleCount < 2 || target.SampleCount != 1 || target.MipLevels != 1 || target.ArrayLayers != 1 ||
        source.Width != target.Width || source.Height != target.Height)
    {
        Logger::Log::Error("MsaaColorResolvePass '{}': needs a multisampled source and a single-sample, one-mip, "
                           "one-layer target of its size (source {}x{} x{}, target {}x{} x{}, {} mips, {} layers); "
                           "the pass is not declared",
                           passName, source.Width, source.Height, source.SampleCount, target.Width, target.Height,
                           target.SampleCount, target.MipLevels, target.ArrayLayers);
        return {};
    }
    ResolveDeviceResources resources{};
    if (!EnsureResolveResources(*device, resources))
        return {};

    RenderGraph::RGFullscreenTextureInput input{};
    input.Binding = 0;
    input.Texture = multisampled;
    input.Sampler = resources.Sampler;
    input.Required = true;

    // The ACES 2 tables are static engine data, uploaded once, so the graph tracks no access to them.
    RenderGraph::RGFullscreenBufferInput tables{};
    tables.Binding = kAces2TablesBinding;
    tables.Buffer = resources.Aces2Tables;
    tables.Size = GetAces2TablesBufferBytes();
    tables.IsStorage = true;
    std::array<RenderGraph::RGFullscreenBufferInput, 3> buffers{tables, view.ViewParams, view.ExposureHistory};
    buffers[1].Binding = kViewParamsBinding;
    buffers[1].IsStorage = false;
    buffers[2].Binding = kExposureHistoryBinding;
    buffers[2].IsStorage = true;

    RenderGraph::RGFullscreenDesc desc{};
    desc.Name = passName;
    desc.Phase = PassPhase::kWorldRender;
    desc.Pipeline = bloomSampleDelta.IsValid() ? resources.BloomDeltaPipeline : resources.Pipeline;
    desc.Layout = resources.Layout;
    desc.Textures = {&input, 1};
    desc.Buffers = buffers;
    desc.Target = resolved;
    desc.SecondTarget = bloomSampleDelta;
    desc.Ops.Load = RenderGraph::RGLoadOp::DontCare;
    desc.Ops.Store = RenderGraph::RGStoreOp::Store;
    desc.Ops.WritesEveryPixel = true;
    return RenderGraph::AddFullscreenPass(frame, desc);
}

} // namespace GameEngine::Rendering::Passes
