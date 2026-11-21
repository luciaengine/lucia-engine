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
#include "Rendering/Core/RenderGraph/RGFrame.h"

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
    for (auto& [key, textures] : m_Textures)
    {
        if (!m_Device)
            break;
        if (textures.Atlas.Color.IsValid())
            m_Device->DestroyTexture(textures.Atlas.Color);
        if (textures.Atlas.NormalDepth.IsValid())
            m_Device->DestroyTexture(textures.Atlas.NormalDepth);
        if (textures.Atlas.SeasonMask.IsValid())
            m_Device->DestroyTexture(textures.Atlas.SeasonMask);
        if (textures.Atlas.DiffuseTransmission.IsValid())
            m_Device->DestroyTexture(textures.Atlas.DiffuseTransmission);
    }
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

    const bool sourcesSettled = PartsSettled(services, job);
    std::vector<GUID> maskedMaterials;
    if (services.Textures().StreamsMaterialMips() && (sourcesSettled || job.SettledFrames != 0))
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
    const uint32 frameCount = job.Request.Key.FramesPerSide * job.Request.Key.FramesPerSide;
    slot.Commands.reserve(static_cast<size_t>(kFramesPerRenderedFrame) * job.Request.Parts.size());
    slot.PushConstants.reserve(slot.Commands.capacity());

    const RenderGraph::RGTexture color = frame.ImportExternalTexture(
        "Impostor.Color", textures.Atlas.Color, Rendering::ResourceState::ShaderResource,
        Rendering::TextureFormat::RGBA8_UNORM);
    const RenderGraph::RGTexture normalDepth = frame.ImportExternalTexture(
        "Impostor.NormalDepth", textures.Atlas.NormalDepth, Rendering::ResourceState::ShaderResource,
        Rendering::TextureFormat::R16G16B16A16_FLOAT);
    const RenderGraph::RGTexture seasonMask = frame.ImportExternalTexture(
        "Impostor.SeasonMask", textures.Atlas.SeasonMask, Rendering::ResourceState::ShaderResource,
        Rendering::TextureFormat::R8G8_UNORM);
    RenderGraph::RGTexture diffuseTransmission;
    if (job.Request.Key.BakeDiffuseTransmission)
        diffuseTransmission = frame.ImportExternalTexture(
            "Impostor.DiffuseTransmission", textures.Atlas.DiffuseTransmission,
            Rendering::ResourceState::ShaderResource, Rendering::TextureFormat::RGBA8_UNORM);
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
                          p.AttachColor(0, color, clear);
                          p.AttachColor(1, normalDepth, clear);
                          p.AttachColor(2, seasonMask, clear);
                          if (diffuseTransmission.IsValid())
                              p.AttachColor(3, diffuseTransmission, clear);
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
    job.Cleared = true;
    // Leave the cursor at the first missing tile, even if later tiles recorded.
    for (const bool* recorded : pending.RecordedTiles)
    {
        if (!recorded || !*recorded)
            break;
        ++job.NextFrame;
    }
    const uint32 frameCount = job.Request.Key.FramesPerSide * job.Request.Key.FramesPerSide;
    if (job.NextFrame == frameCount)
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

void ImpostorBaker::FreeTextures(Rendering::IDevice* device, const ImpostorAtlasKey& key)
{
    AtlasTextures* textures = m_Textures.Find(key);
    if (!textures)
        return;
    if (device && textures->Atlas.Color.IsValid())
        device->DestroyTexture(textures->Atlas.Color);
    if (device && textures->Atlas.NormalDepth.IsValid())
        device->DestroyTexture(textures->Atlas.NormalDepth);
    if (device && textures->Atlas.SeasonMask.IsValid())
        device->DestroyTexture(textures->Atlas.SeasonMask);
    if (device && textures->Atlas.DiffuseTransmission.IsValid())
        device->DestroyTexture(textures->Atlas.DiffuseTransmission);
    m_Textures.Erase(key);
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
    RenderGraph::RGAttachmentOps keep{};
    targets.AddExtraColor(1u, normalDepth, keep);
    targets.AddExtraColor(2u, seasonMask, keep);
    Rendering::MaterialKeyword keywords = kBakeKeywords;
    if (diffuseTransmission.IsValid())
    {
        targets.AddExtraColor(3u, diffuseTransmission, keep);
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
