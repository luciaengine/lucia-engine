#include "Engine/Rendering/Pipeline/Nodes/DepthResolveNode.h"

#include "Engine/Rendering/Pipeline/Nodes/PipelineNodeUtils.h"
#include "Engine/Rendering/Pipeline/RenderPipeline.h"
#include "Engine/Rendering/RenderServices.h"
#include "../../Forest/ForestPlacementFeature.h"

#include "Logger/Logger.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/Pipeline/PipelineTypes.h"
#include "Rendering/Materials/MaterialHelper.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"

#include <optional>

#include <nlohmann/json.hpp>

namespace GameEngine::Engine::Renderer { using namespace ::GameEngine::Rendering; }

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{
using namespace ::GameEngine::Rendering;

namespace
{
constexpr int32 kForestDepthTileSize = 4;
}

bool DepthResolveNode::Initialize(std::string nodeId, std::string nodeJson, std::string* /*outError*/)
{
    m_Id = std::move(nodeId);
    try
    {
        auto j = nlohmann::json::parse(nodeJson);
        if (j.is_object())
        {
            if (j.contains("output") && j["output"].is_string())
                m_OutputKey = j["output"].get<std::string>();
            if (j.contains("poolName") && j["poolName"].is_string())
                m_PoolPrefix = j["poolName"].get<std::string>();
            m_Farthest = j.value("resolve", std::string{}) == "farthest";
        }
    }
    catch (const std::exception&)
    {
        // Keep defaults on malformed config.
    }
    if (m_OutputKey == Names::View::ForestOcclusionDepth)
    {
        m_Farthest = true;
        m_PoolPrefix = "Pipeline.ForestOcclusionDepth.View";
    }
    return true;
}

void DepthResolveNode::Declare(RenderPipelineInstance& instance,
                               const PipelineDeclareContext& /*ctx*/)
{
    if (auto* dev = instance.GetRenderServices().GetDevice())
        LoadShaders(dev);
}

void DepthResolveNode::RequestPipeline(IDevice& device, uint32_t depthSampleCount)
{
    LoadShaders(&device);
    const ComputePipelineId id = depthSampleCount > 1 ? m_ResolvePipelineId : m_CopyPipelineId;
    if (id.IsValid())
        device.RequestComputePipeline(id);
}

void DepthResolveNode::RequestPausedViewPipelines(const PausedViewPrewarm& prewarm)
{
    // The live declare copies the view depth when the view has one, as the world pass draws into it.
    IDevice* device = prewarm.Services.GetDevice();
    const std::optional<PipelineFormatKey> formats = prewarm.WorldColorTargetsKey();
    if (!device || !formats || formats->DepthFormat == TextureFormat{})
        return;
    RequestPipeline(*device, formats->RasterizationSamples);
}

void DepthResolveNode::DeclareForView(ViewDeclare& d)
{
    if (m_OutputKey == Names::View::ForestOcclusionDepth)
    {
        const auto* forest = d.Services.GetFeature<ForestPlacementFeature>();
        const auto prepassDepth = d.ResolveTexture(Names::View::CameraPrepassDepth);
        if (!forest || !forest->WantsPageOcclusion(d.View.worldId) ||
            !prepassDepth.IsValid() || prepassDepth.Id != d.ViewDepth.Id ||
            d.Services.Views().GetViewLetterbox(d.View.id).active ||
            d.Services.Views().GetViewWorldPassFlipY(d.View.id))
            return;
    }
    const RenderGraph::RGTexture depth = d.ViewDepth; // prepass override already published
    if (!depth.IsValid())
        return;
    const auto& dd = d.Frame.Graph().ResourceDesc(depth.Id);
    const uint32_t tileSize = m_Farthest ? kForestDepthTileSize : 1u;
    const uint32_t w = (dd.Width + tileSize - 1u) / tileSize;
    const uint32_t h = (dd.Height + tileSize - 1u) / tileSize;
    if (w == 0 || h == 0)
        return;
    const auto pipeline = dd.SampleCount > 1 ? m_ResolvePipelineId : m_CopyPipelineId;
    if (!pipeline.IsValid())
        return;
    if (m_Farthest)
    {
        auto* device = d.Frame.Device();
        if (!device || device->RequestComputePipeline(pipeline) != PipelineBuildState::Warm)
            return;
    }

    const std::string poolName = m_PoolPrefix + std::to_string(static_cast<uint32_t>(d.View.id));
    const RenderGraph::RGTexture resolved = ImportResolveTarget(d, poolName, w, h);
    if (!resolved.IsValid())
        return;
    const RenderGraph::RGPass resolvePass =
        DeclareResolvePass(d, d.PassName(m_Farthest ? m_Id.c_str() : "DepthResolve").c_str(), depth, resolved, w, h);
    if (m_Farthest && !resolvePass.IsValid())
        return;
    // View.DepthResolved holds the view depth the colour pass depth-tests against, and every depth reader
    // takes it except the occlusion passes below. Until the non-occluding heads draw, this copy is that depth.
    d.PublishTexture(m_OutputKey, resolved);
    if (!resolvePass.IsValid() || m_OutputKey != Names::View::DepthResolved)
        return;

    // View.OccluderDepthResolved holds the occluding geometry alone, without the non-occluding heads (grass).
    // Only GTAO and the contact shadows read it: the blades receive neither, so they occlude neither. The heads
    // join the view depth only after this copy, and only the instance that publishes View.DepthResolved draws
    // them (not the post-ocean one); a pipeline without it draws them in the camera prepass.
    d.PublishTexture(Names::View::OccluderDepthResolved, resolved);
    const RenderGraph::RGPass nonOccludingPrepass =
        d.Services.AddWorldNonOccludingDepthPrepassForView(d.Frame, d.View.id, depth);
    if (!nonOccludingPrepass.IsValid())
        return;

    // The heads are now in the view depth, so View.DepthResolved is a second copy taken after them. A reader
    // that measured distance or decided visibility from a depth without the blades would treat them as absent.
    const RenderGraph::RGTexture withHeads = ImportResolveTarget(d, poolName + ".NonOccludingHeads", w, h);
    if (!withHeads.IsValid())
        return;
    if (DeclareResolvePass(d, d.PassName("DepthResolveNonOccludingHeads").c_str(), depth, withHeads, w, h)
            .IsValid())
        d.PublishTexture(m_OutputKey, withHeads);
}

RenderGraph::RGTexture DepthResolveNode::ImportResolveTarget(ViewDeclare& d, const std::string& poolName,
                                                             uint32_t width, uint32_t height) const
{
    // POOL import, not a transient: the resolved depth enters the world
    // pass's declaration-time binding table (ge_sceneDepth), which needs the
    // physical at declaration.
    Rendering::TextureDesc td{};
    td.width = width;
    td.height = height;
    td.depth = 1;
    td.mipLevels = 1;
    td.arrayLayers = 1;
    td.sampleCount = 1;
    td.format = static_cast<uint32_t>(TextureFormat::R32_FLOAT);
    td.usage = static_cast<uint32_t>(TextureUsage::UnorderedAccess | TextureUsage::ShaderResource);
    // GENERAL-resident: the HZB compute pass storage-reads this while AO / world /
    // fog / etc sample it. The GENERAL sampled-descriptor claim (mirrored into
    // the RG as RGResourceDesc::SampledInGeneralLayout) keeps every sampled read
    // in GENERAL, stopping the cross-queue layout ping-pong that trips
    // VUID-09600 / VUID-09675 with async compute — sampling from GENERAL is legal.
    td.sampledInGeneralLayout = true;
    td.debugName = poolName.c_str();
    return d.Frame.ImportPersistentTexture(poolName.c_str(), td);
}

RenderGraph::RGPass DepthResolveNode::DeclareResolvePass(ViewDeclare& d, const char* passName,
                                                         RenderGraph::RGTexture depth, RenderGraph::RGTexture target,
                                                         uint32_t width, uint32_t height)
{
    return d.Frame.AddComputePass(
        passName, Rendering::PassPhase::kDefault,
        [&](RenderGraph::RGPassBuilder& p)
        {
            p.Prepare([this, depth](RenderGraph::RGPrepareContext& context)
                      {
                          IDevice* device = context.GetDevice();
                          const TextureHandle source = context.GetTexture(depth);
                          if (!device || !source.IsValid())
                              return false;
                          const bool multisampled = device->GetTextureSampleCount(source) > 1u;
                          return context.PreparePipeline(multisampled ? m_ResolvePipelineId : m_CopyPipelineId).IsValid();
                      });
            p.Read(depth, RenderGraph::RGTextureRead::Sampled);
            p.Write(target, RenderGraph::RGTextureWrite::Storage);
        },
        [this, depth, target, width, height](RenderGraph::RGContext& ctx)
        { RecordResolve(ctx, depth, target, width, height); });
}

void DepthResolveNode::RecordResolve(RenderGraph::RGContext& ctx, RenderGraph::RGTexture depth,
                                     RenderGraph::RGTexture target, uint32_t width, uint32_t height) const
{
    auto* dev = ctx.GetDevice();
    auto* cl = ctx.Cmd;
    if (!dev || !cl)
        return;
    const auto depthTex = ctx.GetTexture(depth);
    const auto resolvedTex = ctx.GetTexture(target);
    if (!depthTex.IsValid() || !resolvedTex.IsValid())
        return;

    // Variant from the BOUND image's actual sample count — a runtime
    // MSAA change re-specs the depth in place, and the dispatched
    // pipeline must match the physical image (same guard as the old
    // path).
    const bool isMS = dev->GetTextureSampleCount(depthTex) > 1u;
    const ComputePipelineId pipelineId = isMS ? m_ResolvePipelineId : m_CopyPipelineId;
    const ShaderMeta* meta = isMS ? m_ResolveMeta.get() : m_CopyMeta.get();
    const DescriptorSetLayoutDesc& set0Layout = isMS ? m_ResolveSet0Layout : m_CopySet0Layout;
    const char* depthBindingName = isMS ? "uDepthMS" : "uDepth";
    if (!pipelineId.IsValid())
        return;

    // A binding name the reflected shader does not have is not written.
    DescriptorWrite writes[2];
    uint32_t writeCount = 0;
    uint32_t binding = 0;
    DescriptorType reflectedType{};
    if (meta && m_Sampler.IsValid() && Rendering::TryGetSet0BindingByName(*meta, depthBindingName, binding, reflectedType))
        writes[writeCount++] = {
            .Binding = binding, .Type = DescriptorType::CombinedImageSampler, .Texture = depthTex, .Sampler = m_Sampler};
    if (meta && Rendering::TryGetSet0BindingByName(*meta, "uDepthResolved", binding, reflectedType))
        writes[writeCount++] = {.Binding = binding, .Type = DescriptorType::StorageImage, .Texture = resolvedTex};
    const DescriptorSetHandle ds =
        dev->GetOrCreateDescriptorSet(set0Layout, std::span(writes, writeCount), "DepthResolve.Set0");
    if (!ds.IsValid())
        return;

    PipelineHandle pipe = ctx.GetOrCreatePipelineVariant(pipelineId);
    if (!pipe.IsValid())
        return;
    cl->SetPipeline(pipe);
    cl->BindDescriptorSet(0, ds, pipe);
    cl->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
}

void DepthResolveNode::LoadShaders(IDevice* device)
{
    if (m_LoadAttempted)
        return;
    m_LoadAttempted = true;

    auto loadCompute = [&](const char* path,
                           ComputePipelineId& outPipeline,
                           std::unique_ptr<ShaderMeta>& outMeta,
                           DescriptorSetLayoutDesc& outLayout,
                           const char* debugName, bool farthest) -> bool
    {
        ShaderPackage pkg{};
        std::string loadErr;
        if (!LoadShaderPkg(path, device->PreferredShaderSource(), pkg, &loadErr))
        {
            LOG_WARNING("DepthResolveNode: failed to load {}: {}", path, loadErr);
            return false;
        }
        auto itCs = pkg.stageBytes.find("cs");
        if (itCs == pkg.stageBytes.end() || itCs->second.empty())
        {
            LOG_WARNING("DepthResolveNode: {} missing cs stage", path);
            return false;
        }

        outMeta = std::make_unique<ShaderMeta>(std::move(pkg.meta));

        ComputePipelineDesc cd{};
        cd.ComputeShader = std::make_shared<const std::vector<uint8_t>>(std::move(itCs->second));
        cd.DebugName = debugName;
        if (farthest)
            cd.Specialization.emplace().AddConstant(0u, kForestDepthTileSize);

        outLayout = DescriptorSetLayoutDesc{};
        if (outMeta)
        {
            auto patchLayout = [&](uint32_t setIndex, DescriptorSetLayoutDesc& dsl) {
                if (setIndex == 0)
                    outLayout = dsl;
            };
            std::string err;
            MaterialHelper::ApplyShaderMetaToComputeDesc(
                *device, *outMeta, cd,
                MaterialBuilder::MergeMode::Auto,
                {true, 128}, patchLayout, &err);
        }

        outPipeline = device->InternComputePipeline(std::move(cd));
        return true;
    };

    loadCompute("Shaders/depth_resolve.shaderpkg",
                m_ResolvePipelineId, m_ResolveMeta, m_ResolveSet0Layout, "DepthResolve_MSAA", m_Farthest);
    loadCompute("Shaders/depth_copy.shaderpkg",
                m_CopyPipelineId, m_CopyMeta, m_CopySet0Layout, "DepthResolve_Copy", m_Farthest);

    if (!m_Sampler.IsValid())
    {
        m_Sampler = device->CreateSampler(
            SamplerDesc::MaterialLinearClamp("DepthResolve.Sampler"));
    }
}

void InitializePostWorldDepthResolve(DepthResolveNode& resolve, std::string nodeId)
{
    resolve.Initialize(std::move(nodeId),
                       std::string(R"({"output":")") + Names::View::DepthResolvedPostOcean +
                           R"(","poolName":"Pipeline.DepthResolvedPostOcean.View"})",
                       nullptr);
}

RenderGraph::RGTexture DeclarePostWorldDepth(ViewDeclare& d, DepthResolveNode& resolve)
{
    RenderGraph::RGTexture depth = d.ResolveTexture(Names::View::DepthResolvedPostOcean);
    if (depth.IsValid())
        return depth;
    resolve.DeclareForView(d);
    return d.ResolveTexture(Names::View::DepthResolvedPostOcean);
}

} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes
