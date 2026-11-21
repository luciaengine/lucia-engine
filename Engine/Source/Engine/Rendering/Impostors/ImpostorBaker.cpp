#include "Engine/Rendering/Impostors/ImpostorBaker.h"

#include "../IdentityMeshDrawConstants.h"

#include "Engine/Rendering/DrawCommand.h"
#include "Engine/Rendering/Impostors/ImpostorFrameCamera.h"
#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/MaterialRegistry.h"
#include "Engine/Rendering/MaterialSystem.h"
#include "Engine/Rendering/PipelineVariantCache.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/TextureService.h"
#include "Engine/Rendering/ViewRegistry.h"
#include "Logger/Logger.h"
#include "Rendering/Core/PassPhase.h"
#include "Rendering/Core/Pipeline/PipelineDescTranslator.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Core/RenderGraph/RGFullscreen.h"
#include "Rendering/Materials/Shader/ShaderLayoutShape.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"

#include <algorithm>
#include <bit>
#include <utility>

namespace GameEngine::Engine::Renderer
{
namespace
{

namespace RenderGraph = Rendering::RenderGraph;

constexpr Rendering::MaterialKeyword kBakeKeywords = Rendering::MaterialKeyword::ImpostorBake;
// Frames in a row with no pipeline build in flight before a job bakes: the draws of the
// warm-up frame have requested every variant the bake needs by then.
constexpr uint32 kSettleFrames = 2;
constexpr uint64 kAllocationRetryDelay = 120;

uint32 AtlasSide(const ImpostorAtlasKey& key)
{
    return key.FramesPerSide * key.FrameSize;
}

} // namespace

ImpostorBaker::ImpostorBaker(uint64 budgetBytes)
    : m_Cache(budgetBytes)
{
}

ImpostorBaker::~ImpostorBaker()
{
    if (m_Device)
        for (auto& [key, textures] : m_Textures)
            DestroyAtlas(*m_Device, textures);
}

ImpostorAtlas ImpostorBaker::Request(const ImpostorBakeRequest& request, uint64 frame)
{
    std::lock_guard lock(m_Mutex);
    if (const uint64* retry = m_AllocationRetryFrames.Find(request.Key); retry && frame < *retry)
        return {};
    const ImpostorAtlasCache::State state = m_Cache.Request(request.Key, frame, m_Evicted);
    if (state == ImpostorAtlasCache::State::Ready)
    {
        const AtlasTextures* textures = m_Textures.Find(request.Key);
        return textures ? textures->Atlas : ImpostorAtlas{};
    }
    if (state == ImpostorAtlasCache::State::Pending && !m_Textures.Contains(request.Key))
    {
        const bool queued = std::any_of(m_Jobs.begin(), m_Jobs.end(),
                                        [&request](const BakeJob& job) { return job.Request.Key == request.Key; });
        if (!queued)
        {
            m_Jobs.push_back(BakeJob{request});
            Logger::Log::Debug("Impostor atlas queued: mesh={} tile={} frame={}",
                               request.Key.MeshHash, request.Key.FrameSize, frame);
        }
    }
    return {};
}

void ImpostorBaker::DeclareFrameStage(RenderGraph::RGFrame& frame, RenderServices& services)
{
    std::lock_guard lock(m_Mutex);
    Rendering::IDevice* device = services.GetDevice();
    if (!device || m_PendingBake.Frame)
        return;
    m_Device = device;
    const uint64 frameEpoch = services.Spine().WorldFrameEpoch();
    m_AllocationRetryFrames.EraseIf([frameEpoch](const ImpostorAtlasKey&, uint64 retry)
    {
        return frameEpoch >= retry;
    });

    // Eviction can happen after the last tile was declared but before its
    // completion is published. Dispose of those textures first so a stale
    // completion cannot mark a readmitted key ready while it has no atlas.
    for (const ImpostorAtlasKey& key : m_Evicted)
    {
        FreeTextures(device, key);
        std::erase_if(m_Jobs, [&key](const BakeJob& job) { return job.Request.Key == key; });
    }
    m_Evicted.clear();

    for (const ImpostorAtlasKey& key : m_Finished)
    {
        if (AtlasTextures* textures = m_Textures.Find(key))
        {
            textures->Ready = true;
            m_Cache.MarkReady(key);
        }
    }
    m_Finished.clear();

    std::erase_if(m_Jobs, [&](const BakeJob& job)
    {
        const ImpostorAtlasKey& key = job.Request.Key;
        if (m_Cache.HasRecentPendingRequest(key, frameEpoch))
            return false;
        FreeTextures(device, key);
        m_Cache.ReleasePending(key);
        return true;
    });
    if (m_Jobs.empty() || !EnsureBakeView(services))
    {
        services.Textures().PrepareImpostorOpacityMips({});
        return;
    }
    BakeJob& job = m_Jobs.front();

    const uint32 frameCount = job.Request.Key.FramesPerSide * job.Request.Key.FramesPerSide;
    const uint32 mipCount = ImpostorAtlasMipCount(job.Request.Key);
    const bool bakingMips = job.NextFrame == frameCount;
    const bool sourcesSettled = !bakingMips && PartsSettled(services, job);
    std::vector<GUID> maskedMaterials;
    if (!bakingMips && services.Textures().StreamsMaterialMips() && (sourcesSettled || job.SettledFrames != 0))
    {
        for (const ImpostorBakePart& part : job.Request.Parts)
        {
            const Material* material = services.Materials().Registry().Find(part.Material);
            if (material && material->GetAlphaMode() == MaterialAlphaMode::Mask)
                maskedMaterials.push_back(part.Material);
        }
    }
    const bool opacityReady = services.Textures().PrepareImpostorOpacityMips(maskedMaterials);
    const bool partsSettled = sourcesSettled && opacityReady;
    AtlasTextures& textures = EnsureTextures(*device, job.Request.Key);
    if (!textures.Atlas.Color.IsValid() || !textures.Atlas.NormalDepth.IsValid() ||
        !textures.Atlas.SeasonMask.IsValid() ||
        (job.Request.Key.BakeDiffuseTransmission && !textures.Atlas.DiffuseTransmission.IsValid()))
    {
        // Release partial allocations so the next bake can use their budget.
        m_AllocationRetryFrames.InsertOrAssign(job.Request.Key, frameEpoch + kAllocationRetryDelay);
        FreeTextures(device, job.Request.Key);
        m_Cache.ReleasePending(job.Request.Key);
        m_Jobs.pop_front();
        return;
    }

    FrameSlot& slot = m_FrameSlots[frame.FrameIndex() % kFrameSlots];
    slot.Commands.clear();
    slot.PushConstants.clear();
    slot.Commands.reserve(static_cast<size_t>(kFramesPerRenderedFrame) * job.Request.Parts.size());
    slot.PushConstants.reserve(slot.Commands.capacity());

    const RenderGraph::RGTexture color = frame.ImportExternalTexture(
        "Impostor.Color", textures.Atlas.Color, Rendering::ResourceState::ShaderResource,
        Rendering::TextureFormat::RGBA8_UNORM, mipCount);
    const RenderGraph::RGTexture normalDepth = frame.ImportExternalTexture(
        "Impostor.NormalDepth", textures.Atlas.NormalDepth, Rendering::ResourceState::ShaderResource,
        Rendering::TextureFormat::R16G16B16A16_FLOAT, mipCount);
    const RenderGraph::RGTexture seasonMask = frame.ImportExternalTexture(
        "Impostor.SeasonMask", textures.Atlas.SeasonMask, Rendering::ResourceState::ShaderResource,
        Rendering::TextureFormat::R8G8_UNORM, mipCount);
    RenderGraph::RGTexture diffuseTransmission;
    if (job.Request.Key.BakeDiffuseTransmission)
        diffuseTransmission = frame.ImportExternalTexture(
            "Impostor.DiffuseTransmission", textures.Atlas.DiffuseTransmission,
            Rendering::ResourceState::ShaderResource, Rendering::TextureFormat::RGBA8_UNORM, mipCount);
    if (bakingMips)
    {
        if (!DeclareMip(frame, services, textures, job, {color, normalDepth, seasonMask, diffuseTransmission}))
        {
            m_Finished.push_back(job.Request.Key);
            m_Jobs.pop_front();
        }
        return;
    }

    // The world pass binds its depth target as ge_sceneDepth at declaration, so the depth must
    // come from the persistent pool, not a transient. Each atlas frame clears it.
    Rendering::TextureDesc depthDesc{};
    depthDesc.width = AtlasSide(job.Request.Key);
    depthDesc.height = depthDesc.width;
    depthDesc.format = static_cast<uint32_t>(services.GetDepthFormat());
    depthDesc.usage = static_cast<uint32_t>(Rendering::TextureUsage::DepthStencil) |
                      static_cast<uint32_t>(Rendering::TextureUsage::ShaderResource);
    depthDesc.debugName = "Impostor.Depth";
    const RenderGraph::RGTexture depth = frame.ImportPersistentTexture("Impostor.Depth", depthDesc);
    if (!depth.IsValid())
        return;

    if (!job.Cleared)
    {
        frame.AddPass("Impostor.ClearAtlas", Rendering::PassPhase::kEarlySetup,
                      [color, normalDepth, seasonMask, diffuseTransmission](RenderGraph::RGPassBuilder& p)
                      {
                          RenderGraph::RGAttachmentOps clear{};
                          clear.Load = RenderGraph::RGLoadOp::Clear;
                          constexpr RenderGraph::RGRange kBaseMip{0, 1, 0, 1};
                          p.AttachColor(0, color, clear, kBaseMip);
                          p.AttachColor(1, normalDepth, clear, kBaseMip);
                          p.AttachColor(2, seasonMask, clear, kBaseMip);
                          if (diffuseTransmission.IsValid())
                              p.AttachColor(3, diffuseTransmission, clear, kBaseMip);
                      },
                      [](RenderGraph::RGContext&) {});
    }

    PendingBake pending{&frame, frame.FrameIndex(), {}, !partsSettled && job.SettledFrames == 0};
    // Prepare the exact bake variants while sources settle, leaving the atlas untouched.
    if (!partsSettled)
    {
        DeclareAtlasFrame(frame, services, job, color, normalDepth, seasonMask,
                          diffuseTransmission, depth, job.NextFrame, slot, true);
    }
    else
    {
        for (uint32 tile = 0; tile < kFramesPerRenderedFrame && job.NextFrame + tile < frameCount; ++tile)
        {
            const bool* recorded = DeclareAtlasFrame(frame, services, job, color, normalDepth, seasonMask,
                                                     diffuseTransmission, depth, job.NextFrame + tile, slot, false);
            if (!recorded)
                break;
            pending.RecordedTiles[tile] = recorded;
        }
    }
    frame.MarkOutput(color, RenderGraph::RGImageLayout::ShaderReadOnly);
    frame.MarkOutput(normalDepth, RenderGraph::RGImageLayout::ShaderReadOnly);
    frame.MarkOutput(seasonMask, RenderGraph::RGImageLayout::ShaderReadOnly);
    if (diffuseTransmission.IsValid())
        frame.MarkOutput(diffuseTransmission, RenderGraph::RGImageLayout::ShaderReadOnly);

    m_PendingBake = pending;
}

void ImpostorBaker::OnFrameSubmittedRG(RenderGraph::RGFrame& frame, const Rendering::IDevice::GpuSyncToken& token)
{
    std::lock_guard lock(m_Mutex);
    if (m_PendingBake.Frame != &frame)
        return;
    const PendingBake pending = std::exchange(m_PendingBake, {});
    if (pending.FrameIndex != frame.FrameIndex() || !token.IsValid() || m_Jobs.empty() ||
        frame.Graph().Submissions().empty() || frame.Stats().SubmissionsMade != frame.Graph().Submissions().size())
        return;

    BakeJob& job = m_Jobs.front();
    const uint32 mipCount = ImpostorAtlasMipCount(job.Request.Key);
    if (pending.RecordedMip != 0)
    {
        if (pending.RecordedMip != job.NextMip || pending.RecordedMip >= mipCount)
            return;
        const size_t layerCount = job.Request.Key.BakeDiffuseTransmission ? 4 : 3;
        for (const bool* recorded : std::span(pending.RecordedMipLayers).first(layerCount))
            if (!recorded || !*recorded)
                return;
        AtlasTextures* textures = m_Textures.Find(job.Request.Key);
        if (!textures || pending.RecordedMip != textures->Atlas.HighestReadyMip + 1)
            return;
        textures->Atlas.HighestReadyMip = pending.RecordedMip;
        ++job.NextMip;
        if (job.NextMip == mipCount)
        {
            m_Finished.push_back(job.Request.Key);
            m_Jobs.pop_front();
        }
        return;
    }

    job.Cleared = true;
    // Leave the cursor at the first missing tile, even if later tiles recorded.
    for (const bool* recorded : pending.RecordedTiles)
    {
        if (!recorded || !*recorded)
            break;
        ++job.NextFrame;
    }
    const uint32 frameCount = job.Request.Key.FramesPerSide * job.Request.Key.FramesPerSide;
    if (job.NextFrame == frameCount && mipCount == 1)
    {
        m_Finished.push_back(job.Request.Key);
        m_Jobs.pop_front();
    }
    else if (pending.Yield && m_Jobs.size() > 1)
    {
        // Tile zero requests the pipelines. Other jobs can bake while they compile.
        m_Jobs.push_back(std::move(job));
        m_Jobs.pop_front();
    }
}

void ImpostorBaker::OnFrameAbortedRG(RenderGraph::RGFrame& frame)
{
    std::lock_guard lock(m_Mutex);
    if (m_PendingBake.Frame == &frame)
        m_PendingBake = {};
}

void ImpostorBaker::OnFrameStreamRetiredRG(RenderGraph::RGFrame& frame)
{
    OnFrameAbortedRG(frame);
}

void ImpostorBaker::OnDeviceRebuilt(Rendering::IDevice* device)
{
    std::lock_guard lock(m_Mutex);
    // The rebuild freed the textures; the next request of each atlas bakes it again.
    m_Textures.Clear();
    m_Cache.Clear();
    m_Jobs.clear();
    m_PendingBake = {};
    m_Finished.clear();
    m_Evicted.clear();
    m_AllocationRetryFrames.Clear();
    m_MipPipeline = {};
    m_MipPipelineRequested = false;
    m_Device = device;
}

bool ImpostorBaker::EnsureBakeView(RenderServices& services)
{
    if (m_View != Rendering::ViewId{})
        return true;
    ViewRegistry& views = services.Views();
    m_Camera = views.AllocateCamera("ImpostorBake");
    m_View = views.AllocateView("ImpostorBake", m_Camera);
    // The bake records its own passes: the render pipeline, culling, extraction and shadows skip the view.
    views.SetViewActiveRenderPipeline(m_View, false);
    views.SetViewWorldPaused(m_View, true);
    views.SetViewRenderLayerMask(m_View, Rendering::RenderLayerMask::None());
    return m_View != Rendering::ViewId{};
}

ImpostorBaker::AtlasTextures& ImpostorBaker::EnsureTextures(Rendering::IDevice& device, const ImpostorAtlasKey& key)
{
    if (AtlasTextures* existing = m_Textures.Find(key))
        return *existing;
    const uint32 side = AtlasSide(key);
    Rendering::TextureDesc desc{};
    desc.width = side;
    desc.height = side;
    desc.mipLevels = ImpostorAtlasMipCount(key);
    desc.usage = static_cast<uint32_t>(Rendering::TextureUsage::RenderTarget) |
                 static_cast<uint32_t>(Rendering::TextureUsage::ShaderResource);
    desc.initialState = Rendering::ResourceState::ShaderResource;
    AtlasTextures textures{};
    desc.format = static_cast<uint32_t>(Rendering::TextureFormat::RGBA8_UNORM);
    desc.debugName = "Impostor.Color";
    textures.Atlas.Color = device.CreateTexture(desc);
    desc.format = static_cast<uint32_t>(Rendering::TextureFormat::R16G16B16A16_FLOAT);
    desc.debugName = "Impostor.NormalDepth";
    textures.Atlas.NormalDepth = device.CreateTexture(desc);
    desc.format = static_cast<uint32_t>(Rendering::TextureFormat::R8G8_UNORM);
    desc.debugName = "Impostor.SeasonMask";
    textures.Atlas.SeasonMask = device.CreateTexture(desc);
    if (key.BakeDiffuseTransmission)
    {
        desc.format = static_cast<uint32_t>(Rendering::TextureFormat::RGBA8_UNORM);
        desc.debugName = "Impostor.DiffuseTransmission";
        textures.Atlas.DiffuseTransmission = device.CreateTexture(desc);
    }
    return m_Textures.InsertOrAssign(key, textures);
}

void ImpostorBaker::DestroyAtlas(Rendering::IDevice& device, AtlasTextures& textures)
{
    for (const auto& views : textures.SampleViews)
        for (auto view : views)
            if (view.IsValid())
                device.DestroyTextureView(view);
    const auto& atlas = textures.Atlas;
    for (auto texture : {atlas.Color, atlas.NormalDepth, atlas.SeasonMask, atlas.DiffuseTransmission})
        if (texture.IsValid())
            device.DestroyTexture(texture);
}

void ImpostorBaker::FreeTextures(Rendering::IDevice* device, const ImpostorAtlasKey& key)
{
    AtlasTextures* textures = m_Textures.Find(key);
    if (!textures)
        return;
    if (device)
        DestroyAtlas(*device, *textures);
    m_Textures.Erase(key);
}

bool ImpostorBaker::EnsureMipPipeline(Rendering::IDevice& device)
{
    using namespace Rendering;
    if (m_MipPipelineRequested)
        return m_MipPipeline.IsValid();
    m_MipPipelineRequested = true;
    ShaderPackage package{};
    if (!LoadShaderPkg("Shaders/impostor_mip.shaderpkg", device.PreferredShaderSource(), package))
        return false;
    const auto vertex = package.stageBytes.find("vs");
    const auto fragment = package.stageBytes.find("fs");
    if (vertex == package.stageBytes.end() || fragment == package.stageBytes.end() ||
        vertex->second.empty() || fragment->second.empty())
        return false;

    m_MipLayout = {};
    m_MipLayout.debugName = "Impostor.MipSet";
    m_MipLayout.bindings = {{0, DescriptorType::CombinedImageSampler, 1, kShaderStageFragment}};
    ApplyMetaImageShapeToLayout(package.meta, m_MipLayout);
    auto pipeline = RenderGraph::MakeFullscreenPipelineDesc("Impostor.Mip");
    pipeline.vertexShader = vertex->second;
    pipeline.pixelShader = fragment->second;
    pipeline.descriptorSetLayouts.push_back(m_MipLayout);
    m_MipPipeline = PipelineDescTranslator::InternGraphics(device, pipeline);
    return m_MipPipeline.IsValid();
}

bool ImpostorBaker::DeclareMip(RenderGraph::RGFrame& frame, RenderServices& services, AtlasTextures& textures,
                              const BakeJob& job, const std::array<RenderGraph::RGTexture, 4>& resources)
{
    using namespace Rendering;
    IDevice* device = services.GetDevice();
    if (!device || !EnsureMipPipeline(*device))
        return false;
    const auto sampler = services.Textures().GetSampler(SamplerPreset::PointClamp);
    if (!sampler.IsValid())
        return false;
    const uint32 mip = job.NextMip;
    if (mip == 0 || mip >= ImpostorAtlasMipCount(job.Request.Key))
        return false;
    const uint32 extent = AtlasSide(job.Request.Key) >> mip;
    const uint32 layers = job.Request.Key.BakeDiffuseTransmission ? 4 : 3;
    const auto& atlas = textures.Atlas;
    const std::array images{atlas.Color, atlas.NormalDepth, atlas.SeasonMask, atlas.DiffuseTransmission};
    const RenderGraph::RGRange input{mip - 1, 1, 0, 1};
    const RenderGraph::RGRange output{mip, 1, 0, 1};
    for (uint32 layer = 0; layer < layers; ++layer)
    {
        if (!resources[layer].IsValid())
            return false;
        auto& views = textures.SampleViews[layer];
        views.resize(mip);
        auto& view = views.back();
        if (!view.IsValid())
            view = device->CreateTextureView(images[layer], {.baseMip = mip - 1, .levelCount = 1, .layerCount = 1});
        if (!view.IsValid())
            return false;
    }

    PendingBake pending{&frame, frame.FrameIndex()};
    pending.RecordedMip = mip;
    for (uint32 layer = 0; layer < layers; ++layer)
    {
        const auto image = resources[layer];
        const auto view = textures.SampleViews[layer].back();
        auto* recorded = frame.AllocatePassData<bool>(false);
        const auto pass = frame.AddPass("Impostor.FilterMip", PassPhase::kFinalize,
            [image, input, output](RenderGraph::RGPassBuilder& builder)
            {
                builder.Read(image, RenderGraph::RGTextureRead::Sampled, input);
                RenderGraph::RGAttachmentOps ops{};
                ops.Load = RenderGraph::RGLoadOp::DontCare;
                builder.AttachColor(0, image, ops, output);
            },
            [this, view, sampler, extent, recorded](RenderGraph::RGContext& context)
            {
                auto* device = context.GetDevice();
                if (!device || !context.Cmd)
                    return;
                const auto pipeline = context.GetOrCreatePipelineVariant(m_MipPipeline);
                if (!pipeline.IsValid())
                    return;
                DescriptorSetDesc descriptor{};
                descriptor.layout = m_MipLayout;
                descriptor.transient = true;
                descriptor.debugName = "Impostor.MipSet";
                const auto set = device->CreateDescriptorSet(descriptor);
                if (!set.IsValid())
                    return;
                device->UpdateCombinedImageSamplerBinding(set, 0, view, sampler);
                context.Cmd->SetPipeline(pipeline);
                context.Cmd->SetViewport(0.0f, 0.0f, static_cast<float>(extent), static_cast<float>(extent));
                context.Cmd->SetScissor(0, 0, extent, extent);
                context.Cmd->BindDescriptorSet(0, set, pipeline);
                context.Cmd->Draw(3, 1);
                *recorded = true;
            });
        if (!pass.IsValid())
            return false;
        pending.RecordedMipLayers[layer] = recorded;
        frame.MarkOutput(image, RenderGraph::RGImageLayout::ShaderReadOnly);
    }
    m_PendingBake = pending;
    return true;
}

bool ImpostorBaker::PartsSettled(RenderServices& services, BakeJob& job)
{
    bool changed = false;
    bool building = false;
    job.SourceRevisions.resize(job.Request.Parts.size());
    for (size_t i = 0; i < job.Request.Parts.size(); ++i)
    {
        const Material* material = services.Materials().Registry().Find(job.Request.Parts[i].Material);
        const uint64 revision = material ? (static_cast<uint64>(material->GetVersion()) << 32u)
                                           | material->GetContentRevision() : 0u;
        changed |= job.SourceRevisions[i] != revision;
        job.SourceRevisions[i] = revision;
        building |= !material || services.Materials().Variants().HasPipelineBuildsInFlight(*material);
    }
    if (changed)
    {
        // All tiles must see one set of source bindings, including resident mips.
        job.NextFrame = 0;
        job.SettledFrames = 0;
        job.Cleared = false;
    }
    if (building)
        job.SettledFrames = 0;
    else if (job.SettledFrames < kSettleFrames)
        ++job.SettledFrames;
    return job.SettledFrames >= kSettleFrames;
}

const bool* ImpostorBaker::DeclareAtlasFrame(RenderGraph::RGFrame& frame, RenderServices& services, const BakeJob& job,
                                      RenderGraph::RGTexture color, RenderGraph::RGTexture normalDepth,
                                      RenderGraph::RGTexture seasonMask, RenderGraph::RGTexture diffuseTransmission,
                                      RenderGraph::RGTexture depth,
                                      uint32 atlasFrame, FrameSlot& slot, bool warmupOnly)
{
    const ImpostorAtlasKey& key = job.Request.Key;
    const uint32 column = atlasFrame % key.FramesPerSide;
    const uint32 row = atlasFrame / key.FramesPerSide;

    ViewRegistry& views = services.Views();
    const Rendering::CameraData camera =
        MakeImpostorFrameCamera(key.Layout, column, row, key.FramesPerSide, job.Request.Center, job.Request.Radius);
    const float radius = job.Request.Radius;
    views.SetCameraData(m_Camera, camera, Rendering::CameraRanges{radius, 3.0f * radius});
    views.SetViewLetterbox(m_View, ViewLetterbox{true, column * key.FrameSize, row * key.FrameSize, key.FrameSize, key.FrameSize});

    // Cleared to far for each atlas frame: the frames share no depth.
    frame.AddPass("Impostor.ClearDepth", Rendering::PassPhase::kEarlySetup,
                  [depth](RenderGraph::RGPassBuilder& p)
                  {
                      RenderGraph::RGAttachmentOps clear{};
                      clear.Load = RenderGraph::RGLoadOp::Clear;
                      p.AttachDepth(depth, clear, RenderGraph::RGDepthAccess::ReadWrite);
                  },
                  [](RenderGraph::RGContext&) {});

    const size_t first = slot.Commands.size();
    for (const ImpostorBakePart& part : job.Request.Parts)
    {
        const Material* material = services.Materials().Registry().Find(part.Material);
        if (!material)
            continue;
        const IdentityMeshDrawConstants constants{material->GetGpuSceneMaterialIndex()};
        slot.PushConstants.push_back(std::bit_cast<std::array<std::byte, sizeof(constants)>>(constants));
        DrawCommand command{};
        command.Material = material;
        command.Geometry.Mesh = part.Mesh;
        command.Bindings.PushConstants = std::span<const std::byte>(slot.PushConstants.back());
        slot.Commands.push_back(command);
    }
    if (slot.Commands.size() - first != job.Request.Parts.size() || first == slot.Commands.size())
        return nullptr;

    RenderServices::WorldPassTargetsRG targets{};
    targets.Color = color;
    targets.Depth = depth;
    constexpr RenderGraph::RGRange kBaseMip{0, 1, 0, 1};
    targets.ColorRange = kBaseMip;
    RenderGraph::RGAttachmentOps keep{};
    targets.AddExtraColor(1u, normalDepth, keep, {}, kBaseMip);
    targets.AddExtraColor(2u, seasonMask, keep, {}, kBaseMip);
    Rendering::MaterialKeyword keywords = kBakeKeywords;
    if (diffuseTransmission.IsValid())
    {
        targets.AddExtraColor(3u, diffuseTransmission, keep, {}, kBaseMip);
        keywords |= Rendering::MaterialKeyword::ImpostorBakeTransmission;
    }
    auto* completion = frame.AllocatePassData<RenderServices::ForwardCommandPassCompletion>();
    completion->WarmupOnly = warmupOnly;
    const auto pass = services.AddForwardCommandPassForView(
        frame, m_View, targets, keywords, std::span<const DrawCommand>(slot.Commands).subspan(first), {},
        "Impostor.BakeFrame", {}, RenderGraph::RGDepthAccess::ReadWrite, completion);
    return pass.IsValid() ? &completion->Recorded : nullptr;
}

} // namespace GameEngine::Engine::Renderer
