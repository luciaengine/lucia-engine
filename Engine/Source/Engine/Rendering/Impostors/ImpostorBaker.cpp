#include "Engine/Rendering/Impostors/ImpostorBaker.h"

#include "Engine/Rendering/DrawCommand.h"
#include "Engine/Rendering/Impostors/ImpostorFrameCamera.h"
#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/MaterialRegistry.h"
#include "Engine/Rendering/MaterialSystem.h"
#include "Engine/Rendering/PipelineVariantCache.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/ViewRegistry.h"
#include "Rendering/Core/PassPhase.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"

#include <algorithm>
#include <cstring>

namespace GameEngine::Engine::Renderer
{
namespace
{

namespace RenderGraph = Rendering::RenderGraph;

constexpr Rendering::MaterialKeyword kBakeKeywords = Rendering::MaterialKeyword::ImpostorBake;
// Frames in a row with no pipeline build in flight before a job bakes: the draws of the
// warm-up frame have requested every variant the bake needs by then.
constexpr uint32 kSettleFrames = 2;

// The vertex adapter's push-constant block on the non-instanced path (adapter_vertex.glsl, PC).
struct BakePushConstants
{
    float Model[16];
    float NormalColumn0[4];
    float NormalColumn1[4];
    float NormalColumn2[4];
    float Extra[4]; // x = material index bits, y = skin palette offset bits.
};
static_assert(sizeof(BakePushConstants) == 128, "BakePushConstants must match adapter_vertex.glsl's PC block");

std::array<std::byte, 128> MakePushConstants(uint32 materialIndex)
{
    BakePushConstants push{};
    // The parts draw untransformed: identity model and normal matrices.
    push.Model[0] = push.Model[5] = push.Model[10] = push.Model[15] = 1.0f;
    push.NormalColumn0[0] = 1.0f;
    push.NormalColumn1[1] = 1.0f;
    push.NormalColumn2[2] = 1.0f;
    std::memcpy(&push.Extra[0], &materialIndex, sizeof(materialIndex));
    std::array<std::byte, 128> bytes{};
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
    for (auto& [key, textures] : m_Textures)
    {
        if (!m_Device)
            break;
        if (textures.Atlas.Color.IsValid())
            m_Device->DestroyTexture(textures.Atlas.Color);
        if (textures.Atlas.NormalDepth.IsValid())
            m_Device->DestroyTexture(textures.Atlas.NormalDepth);
    }
}

ImpostorAtlas ImpostorBaker::Request(const ImpostorBakeRequest& request, uint64 frame)
{
    std::lock_guard lock(m_Mutex);
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
            m_Jobs.push_back(BakeJob{request});
    }
    return {};
}

void ImpostorBaker::DeclareFrameStage(RenderGraph::RGFrame& frame, RenderServices& services)
{
    std::lock_guard lock(m_Mutex);
    Rendering::IDevice* device = services.GetDevice();
    if (!device)
        return;
    m_Device = device;

    // The frame that declared a finished bake has executed by now.
    for (const ImpostorAtlasKey& key : m_Finished)
    {
        if (AtlasTextures* textures = m_Textures.Find(key))
            textures->Ready = true;
        m_Cache.MarkReady(key);
    }
    m_Finished.clear();
    for (const ImpostorAtlasKey& key : m_Evicted)
        FreeTextures(device, key);
    m_Evicted.clear();

    if (m_Jobs.empty() || !EnsureBakeView(services))
        return;
    BakeJob& job = m_Jobs.front();
    if (m_Cache.GetState(job.Request.Key) == ImpostorAtlasCache::State::Absent)
    {
        m_Jobs.pop_front(); // Evicted before its bake finished.
        return;
    }

    AtlasTextures& textures = EnsureTextures(*device, job.Request.Key);
    if (!textures.Atlas.Color.IsValid() || !textures.Atlas.NormalDepth.IsValid())
        return;

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

    if (!job.Cleared)
    {
        frame.AddPass("Impostor.ClearAtlas", Rendering::PassPhase::kEarlySetup,
                      [color, normalDepth](RenderGraph::RGPassBuilder& p)
                      {
                          RenderGraph::RGAttachmentOps clear{};
                          clear.Load = RenderGraph::RGLoadOp::Clear;
                          p.AttachColor(0, color, clear);
                          p.AttachColor(1, normalDepth, clear);
                      },
                      [](RenderGraph::RGContext&) {});
        job.Cleared = true;
    }

    // Warm-up: draw frame 0 until no pipeline the bake needs is still building, then bake from 0.
    if (!PartsSettled(services, job))
    {
        DeclareAtlasFrame(frame, services, job, color, normalDepth, 0u, slot);
    }
    else
    {
        for (uint32 drawn = 0; drawn < kFramesPerRenderedFrame && job.NextFrame < frameCount; ++drawn)
            DeclareAtlasFrame(frame, services, job, color, normalDepth, job.NextFrame++, slot);
    }
    frame.MarkOutput(color, RenderGraph::RGImageLayout::ShaderReadOnly);
    frame.MarkOutput(normalDepth, RenderGraph::RGImageLayout::ShaderReadOnly);

    if (job.NextFrame >= frameCount)
    {
        m_Finished.push_back(job.Request.Key);
        m_Jobs.pop_front();
    }
}

void ImpostorBaker::OnDeviceRebuilt(Rendering::IDevice* device)
{
    std::lock_guard lock(m_Mutex);
    // The rebuild freed the textures; the next request of each atlas bakes it again.
    m_Textures.Clear();
    m_Cache.Clear();
    m_Jobs.clear();
    m_Finished.clear();
    m_Evicted.clear();
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
    m_Textures.Erase(key);
}

bool ImpostorBaker::PartsSettled(RenderServices& services, BakeJob& job)
{
    if (job.SettledFrames >= kSettleFrames)
        return true;
    bool building = false;
    for (const ImpostorBakePart& part : job.Request.Parts)
    {
        const Material* material = services.Materials().Registry().Find(part.Material);
        if (!material || services.Materials().Variants().HasPipelineBuildsInFlight(*material))
            building = true;
    }
    job.SettledFrames = building ? 0u : job.SettledFrames + 1u;
    return job.SettledFrames >= kSettleFrames;
}

void ImpostorBaker::DeclareAtlasFrame(RenderGraph::RGFrame& frame, RenderServices& services, const BakeJob& job,
                                      RenderGraph::RGTexture color, RenderGraph::RGTexture normalDepth,
                                      uint32 atlasFrame, FrameSlot& slot)
{
    const ImpostorAtlasKey& key = job.Request.Key;
    const uint32 column = atlasFrame % key.FramesPerSide;
    const uint32 row = atlasFrame / key.FramesPerSide;
    const uint32 side = AtlasSide(key);

    ViewRegistry& views = services.Views();
    const Rendering::CameraData camera =
        MakeImpostorFrameCamera(key.Layout, column, row, key.FramesPerSide, job.Request.Bounds);
    const float radius = job.Request.Bounds.Radius();
    views.SetCameraData(m_Camera, camera, Rendering::CameraRanges{radius, 3.0f * radius});
    views.SetViewLetterbox(m_View, ViewLetterbox{true, column * key.FrameSize, row * key.FrameSize, key.FrameSize, key.FrameSize});

    // A depth target per atlas frame, cleared to far: the frames share no depth.
    Rendering::TextureDesc depthDesc{};
    depthDesc.width = side;
    depthDesc.height = side;
    depthDesc.format = static_cast<uint32_t>(Rendering::TextureFormat::D32_FLOAT);
    depthDesc.usage = static_cast<uint32_t>(Rendering::TextureUsage::DepthStencil);
    depthDesc.debugName = "Impostor.Depth";
    const RenderGraph::RGTexture depth = frame.CreateTexture("Impostor.Depth", depthDesc);
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
        slot.PushConstants.push_back(MakePushConstants(material->GetGpuSceneMaterialIndex()));
        DrawCommand command{};
        command.Material = material;
        command.Geometry.Mesh = part.Mesh;
        command.Bindings.PushConstants = std::span<const std::byte>(slot.PushConstants.back());
        slot.Commands.push_back(command);
    }
    if (slot.Commands.size() == first)
        return;

    RenderServices::WorldPassTargetsRG targets{};
    targets.Color = color;
    targets.Depth = depth;
    RenderGraph::RGAttachmentOps keep{};
    targets.AddExtraColor(1u, normalDepth, keep);
    services.AddForwardCommandPassForView(frame, m_View, targets, kBakeKeywords,
                                          std::span<const DrawCommand>(slot.Commands).subspan(first), {},
                                          "Impostor.BakeFrame");
}

} // namespace GameEngine::Engine::Renderer
