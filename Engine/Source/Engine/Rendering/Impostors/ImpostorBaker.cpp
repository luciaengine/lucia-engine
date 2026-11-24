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
#include "Rendering/Core/GPUInstanceDepthClass.h"
#include "Rendering/Core/PassPhase.h"
#include "Rendering/Core/Pipeline/PipelineDescTranslator.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Core/RenderGraph/RGFullscreen.h"
#include "Rendering/Materials/Shader/ShaderLayoutShape.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"

#include <algorithm>
#include <cstring>
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
constexpr uint32 kMaxMipAttemptsWithoutProgress = 8;

// The parts draw untransformed, with the non-instanced push constants of adapter_vertex.glsl.
// instanceFlags: kInstanceFlagDepthDoubleSided for a part drawn on both faces, which bakes its
// normal by the two-sided rule (impostor_bake_normal.glsl).
std::array<std::byte, sizeof(IdentityMeshDrawConstants)> MakePushConstants(uint32 materialIndex, uint32 instanceFlags)
{
    IdentityMeshDrawConstants push(materialIndex);
    push.InstanceFlags = instanceFlags;
    std::array<std::byte, sizeof(push)> bytes{};
    std::memcpy(bytes.data(), &push, sizeof(push));
    return bytes;
}

uint32 AtlasSide(const ImpostorAtlasKey& key)
{
    return key.FramesPerSide * key.FrameSize;
}

} // namespace

ImpostorBaker::ImpostorBaker() = default;

ImpostorBaker::~ImpostorBaker()
{
    if (m_Device)
        for (auto& [key, textures] : m_Textures)
            DestroyAtlas(*m_Device, textures);
}

ImpostorAtlas ImpostorBaker::Request(const ImpostorBakeRequest& request, const RenderServices& services)
{
    const uint64 frame = services.Spine().WorldFrameEpoch();
    std::lock_guard lock(m_Mutex);
    const ImpostorAtlasCache::State state = m_Cache.Touch(request.Key, frame);
    if (state == ImpostorAtlasCache::State::Ready)
    {
        const AtlasTextures* textures = m_Textures.Find(request.Key);
        return textures ? textures->Atlas : ImpostorAtlas{};
    }
    if (state == ImpostorAtlasCache::State::Absent)
    {
        if (m_RequestFrame != frame)
        {
            m_Requests.Clear();
            m_RequestFrame = frame;
        }
        if (!m_Requests.Contains(request.Key))
            m_Requests.InsertOrAssign(request.Key, request);
    }
    return {};
}

void ImpostorBaker::AdmitRequests(uint64 frame)
{
    if (m_RequestFrame == frame)
    {
        for (auto& [key, request] : m_Requests)
        {
            if (m_Cache.Request(key, frame, m_Evicted) == ImpostorAtlasCache::State::Pending)
                m_Jobs.push_back(BakeJob{std::move(request)});
        }
    }
    m_Requests.Clear();
}

void ImpostorBaker::DeclareFrameStage(RenderGraph::RGFrame& frame, RenderServices& services)
{
    std::lock_guard lock(m_Mutex);
    Rendering::IDevice* device = services.GetDevice();
    if (!device)
        return;
    m_Device = device;

    // Extraction has marked the atlases in use before any new reservation can evict one.
    AdmitRequests(services.Spine().WorldFrameEpoch());
    if (!m_Evicted.empty())
    {
        std::erase_if(m_Jobs, [this](const BakeJob& job)
        {
            return m_Cache.GetState(job.Request.Key) == ImpostorAtlasCache::State::Absent;
        });
        for (const ImpostorAtlasKey& key : m_Evicted)
            FreeTextures(device, key);
        m_Evicted.clear();
    }

    if (m_PendingBake || m_Jobs.empty() || !EnsureBakeView(services))
        return;
    BakeJob& job = m_Jobs.front();
    if (job.MipAttemptsWithoutProgress >= kMaxMipAttemptsWithoutProgress)
    {
        if (auto* textures = m_Textures.Find(job.Request.Key))
        {
            DestroyAtlas(*device, *textures);
            // An empty entry tells the allocator to use one mip on the next attempt.
            *textures = {};
        }
        job = BakeJob{std::move(job.Request)};
        if (m_Jobs.size() > 1)
        {
            m_Jobs.push_back(std::move(job));
            m_Jobs.pop_front();
        }
        return;
    }

    AtlasTextures& textures = EnsureTextures(*device, job.Request.Key);
    if (!textures.Atlas.Color.IsValid() || !textures.Atlas.NormalDepth.IsValid())
    {
        if (job.Request.Key.FilterMips && m_Jobs.size() > 1)
        {
            m_Jobs.push_back(std::move(job));
            m_Jobs.pop_front();
        }
        return;
    }

    FrameSlot& slot = m_FrameSlots[frame.FrameIndex() % kFrameSlots];
    slot.Commands.clear();
    slot.PushConstants.clear();
    const uint32 frameCount = job.Request.Key.FramesPerSide * job.Request.Key.FramesPerSide;
    slot.Commands.reserve(static_cast<size_t>(kFramesPerRenderedFrame) * job.Request.Parts.size());
    slot.PushConstants.reserve(slot.Commands.capacity());

    const RenderGraph::RGTexture color = frame.ImportExternalTexture(
        "Impostor.Color", textures.Atlas.Color, Rendering::ResourceState::ShaderResource,
        Rendering::TextureFormat::RGBA8_UNORM, textures.MipCount);
    const RenderGraph::RGTexture normalDepth = frame.ImportExternalTexture(
        "Impostor.NormalDepth", textures.Atlas.NormalDepth, Rendering::ResourceState::ShaderResource,
        Rendering::TextureFormat::R16G16B16A16_FLOAT, textures.MipCount);
    if (job.NextFrame == frameCount)
    {
        if (!DeclareMip(frame, services, textures, job.Request.Key, {color, normalDepth}))
            ++job.MipAttemptsWithoutProgress;
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

    PendingBake pending{};
    pending.Frame.Stamp(frame);
    pending.Key = job.Request.Key;
    pending.NextFrame = job.NextFrame;
    pending.SettledFrames = SettledFrames(services, job);
    if (!job.Cleared)
    {
        auto* cleared = frame.AllocatePassData<bool>(false);
        pending.ClearSubmitted = cleared;
        frame.AddPass("Impostor.ClearAtlas", Rendering::PassPhase::kEarlySetup,
                      [color, normalDepth, cleared](RenderGraph::RGPassBuilder& p)
                      {
                          RenderGraph::RGAttachmentOps clear{};
                          p.OnSubmitted([cleared] { *cleared = true; });
                          clear.Load = RenderGraph::RGLoadOp::Clear;
                          constexpr RenderGraph::RGRange baseMip{0, 1, 0, 1};
                          p.AttachColor(0, color, clear, baseMip);
                          p.AttachColor(1, normalDepth, clear, baseMip);
                      },
                      [](RenderGraph::RGContext&) {});
    }

    // Warm-up: draw frame 0 until no pipeline the bake needs is still building, then bake from 0.
    if (pending.SettledFrames < kSettleFrames)
    {
        DeclareAtlasFrame(frame, services, job, color, normalDepth, depth, 0u, slot);
    }
    else
    {
        for (uint32 drawn = 0; drawn < kFramesPerRenderedFrame && pending.NextFrame < frameCount; ++drawn)
        {
            bool* submitted = nullptr;
            if (!job.Request.Parts.empty())
            {
                submitted = frame.AllocatePassData<bool>(false);
                pending.TileSubmitted[drawn] = submitted;
            }
            DeclareAtlasFrame(frame, services, job, color, normalDepth, depth, pending.NextFrame++, slot, submitted);
        }
    }
    frame.MarkOutput(color, RenderGraph::RGImageLayout::ShaderReadOnly);
    frame.MarkOutput(normalDepth, RenderGraph::RGImageLayout::ShaderReadOnly);

    m_PendingBake = pending;
}

void ImpostorBaker::OnFrameSubmittedRG(RenderGraph::RGFrame& frame, const Rendering::IDevice::GpuSyncToken& token)
{
    std::lock_guard lock(m_Mutex);
    if (!m_PendingBake || m_PendingBake->Frame.Frame != &frame)
        return;
    const auto pending = *m_PendingBake;
    m_PendingBake.reset();
    if (!pending.Frame.IsFor(frame) || !token.IsValid() || frame.Graph().Submissions().empty() ||
        frame.Stats().SubmissionsMade != frame.Graph().Submissions().size() ||
        m_Jobs.empty() || m_Jobs.front().Request.Key != pending.Key)
        return;
    auto* textures = m_Textures.Find(pending.Key);
    if (!textures)
        return;
    BakeJob& job = m_Jobs.front();
    if (pending.Level == 0)
    {
        if (!job.Cleared && (!pending.ClearSubmitted || !*pending.ClearSubmitted))
            return;
        job.Cleared = true;
        job.SettledFrames = pending.SettledFrames;
        if (std::ranges::any_of(pending.TileSubmitted, [](const bool* tile) { return tile && !*tile; }))
        {
            // Retrying a partially drawn tile must start from clear content. Other jobs can bake meanwhile.
            job.NextFrame = 0;
            job.Cleared = false;
            if (m_Jobs.size() > 1)
            {
                m_Jobs.push_back(std::move(job));
                m_Jobs.pop_front();
            }
            return;
        }
        job.NextFrame = pending.NextFrame;
        const uint32 frameCount = pending.Key.FramesPerSide * pending.Key.FramesPerSide;
        if (job.NextFrame == frameCount && textures->MipCount == 1)
        {
            m_Cache.MarkReady(pending.Key, 1u);
            m_Jobs.pop_front();
        }
        return;
    }
    if (textures->Atlas.HighestReadyMip + 1 != pending.Level)
        return;
    if (!std::ranges::all_of(pending.Recorded, [](const bool* recorded) { return recorded && *recorded; }))
    {
        ++job.MipAttemptsWithoutProgress;
        return;
    }

    job.MipAttemptsWithoutProgress = 0;
    textures->Atlas.HighestReadyMip = pending.Level;
    if (pending.Level + 1 == textures->MipCount)
    {
        m_Cache.MarkReady(pending.Key, textures->MipCount);
        m_Jobs.pop_front();
    }
}

void ImpostorBaker::OnFrameAbortedRG(RenderGraph::RGFrame& frame)
{
    std::lock_guard lock(m_Mutex);
    if (m_PendingBake && m_PendingBake->Frame.Frame == &frame)
        m_PendingBake.reset();
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
    m_Requests.Clear();
    m_Jobs.clear();
    m_Evicted.clear();
    m_PendingBake.reset();
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
    AtlasTextures* existing = m_Textures.Find(key);
    if (existing && (!key.FilterMips ||
        (existing->Atlas.Color.IsValid() && existing->Atlas.NormalDepth.IsValid())))
        return *existing;

    const uint32 side = AtlasSide(key);
    Rendering::TextureDesc desc{};
    desc.width = side;
    desc.height = side;
    desc.usage = static_cast<uint32_t>(Rendering::TextureUsage::RenderTarget) |
                 static_cast<uint32_t>(Rendering::TextureUsage::ShaderResource);
    desc.initialState = Rendering::ResourceState::ShaderResource;
    AtlasTextures textures{};
    // After a failed allocation, request only base levels. The backend retires
    // released resources between attempts.
    const uint32 requestedMipCount = existing ? 1u : ImpostorAtlasMipCount(key);
    if (requestedMipCount > 1 && EnsureMipPipeline(device))
        textures.MipCount = requestedMipCount;
    desc.mipLevels = textures.MipCount;
    desc.format = static_cast<uint32_t>(Rendering::TextureFormat::RGBA8_UNORM);
    desc.debugName = "Impostor.Color";
    textures.Atlas.Color = device.CreateTexture(desc);
    if (textures.Atlas.Color.IsValid())
    {
        desc.format = static_cast<uint32_t>(Rendering::TextureFormat::R16G16B16A16_FLOAT);
        desc.debugName = "Impostor.NormalDepth";
        textures.Atlas.NormalDepth = device.CreateTexture(desc);
    }
    if (!textures.Atlas.Color.IsValid() || !textures.Atlas.NormalDepth.IsValid())
    {
        DestroyAtlas(device, textures);
        textures.Atlas = {};
    }
    return m_Textures.InsertOrAssign(key, std::move(textures));
}

void ImpostorBaker::DestroyAtlas(Rendering::IDevice& device, AtlasTextures& textures)
{
    for (const auto& views : textures.SampleViews)
        for (auto view : views)
            if (view.IsValid())
                device.DestroyTextureView(view);
    const auto& atlas = textures.Atlas;
    for (auto texture : {atlas.Color, atlas.NormalDepth})
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
                              const ImpostorAtlasKey& key, const std::array<RenderGraph::RGTexture, 2>& resources)
{
    using namespace Rendering;
    IDevice* device = services.GetDevice();
    if (!device || !EnsureMipPipeline(*device))
        return false;
    const auto sampler = services.Textures().GetSampler(SamplerPreset::PointClamp);
    if (!sampler.IsValid())
        return false;
    const uint32 mip = textures.Atlas.HighestReadyMip + 1;
    const uint32 extent = AtlasSide(key) >> mip;
    const auto& atlas = textures.Atlas;
    const std::array images{atlas.Color, atlas.NormalDepth};
    const RenderGraph::RGRange input{mip - 1, 1, 0, 1};
    const RenderGraph::RGRange output{mip, 1, 0, 1};
    for (uint32 layer = 0; layer < images.size(); ++layer)
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

    PendingBake pending{};
    pending.Key = key;
    pending.Level = mip;
    pending.Frame.Stamp(frame);
    for (uint32 layer = 0; layer < images.size(); ++layer)
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
        // Keep the pending frame until submission if the other pass was already added.
        if (!pass.IsValid())
            break;
        pending.Recorded[layer] = recorded;
        frame.MarkOutput(image, RenderGraph::RGImageLayout::ShaderReadOnly);
    }
    m_PendingBake = pending;
    return true;
}

uint32 ImpostorBaker::SettledFrames(RenderServices& services, const BakeJob& job)
{
    if (job.SettledFrames >= kSettleFrames)
        return kSettleFrames;
    bool building = false;
    for (const ImpostorBakePart& part : job.Request.Parts)
    {
        const Material* material = services.Materials().Registry().Find(part.Material);
        if (!material || services.Materials().Variants().HasPipelineBuildsInFlight(*material))
            building = true;
    }
    return building ? 0u : job.SettledFrames + 1u;
}

void ImpostorBaker::DeclareAtlasFrame(RenderGraph::RGFrame& frame, RenderServices& services, const BakeJob& job,
                                      RenderGraph::RGTexture color, RenderGraph::RGTexture normalDepth,
                                      RenderGraph::RGTexture depth, uint32 atlasFrame, FrameSlot& slot, bool* submitted)
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
        if (!material || !part.Mesh.IsValid())
            continue;
        const uint32 flags = material->IsDoubleSided() ? Rendering::kInstanceFlagDepthDoubleSided : 0u;
        slot.PushConstants.push_back(MakePushConstants(material->GetGpuSceneMaterialIndex(), flags));
        DrawCommand command{};
        command.Material = material;
        command.Geometry.Mesh = part.Mesh;
        command.Bindings.PushConstants = std::span<const std::byte>(slot.PushConstants.back());
        slot.Commands.push_back(command);
    }
    if (slot.Commands.size() == first || (submitted && slot.Commands.size() - first != job.Request.Parts.size()))
        return;

    RenderServices::WorldPassTargetsRG targets{};
    targets.Color = color;
    targets.Depth = depth;
    RenderGraph::RGAttachmentOps keep{};
    constexpr RenderGraph::RGRange baseMip{0, 1, 0, 1};
    targets.ColorRange = baseMip;
    targets.AddExtraColor(1u, normalDepth, keep, {}, baseMip);
    services.AddForwardCommandPassForView(frame, m_View, targets, kBakeKeywords,
                                          std::span<const DrawCommand>(slot.Commands).subspan(first), {},
                                          "Impostor.BakeFrame", {}, RenderGraph::RGDepthAccess::ReadWrite, submitted);
}

} // namespace GameEngine::Engine::Renderer
