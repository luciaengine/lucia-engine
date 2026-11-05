#include "ShaderGraph/GraphPreviewAtlas.h"

#include "Engine/Rendering/IEnvironmentSource.h"
#include "Engine/Rendering/ImageBasedLightingFeature.h"
#include "Engine/Rendering/PipelineVariantCache.h"

#include "Components/Rendering/Light.h"
#include "Components/Rendering/LightPhotometry.h"
#include "Engine/Rendering/DrawBindings.h"
#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/MaterialBinder.h"
#include "Engine/Rendering/MaterialRegistry.h"
#include "Engine/Rendering/MaterialSystem.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "Engine/Rendering/PassBindingContext.h"
#include "Engine/Rendering/PrimitiveGenerator.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/ViewRegistry.h"
#include "Mathematics/Matrix4x4.h"
#include "Mathematics/MatrixOps.h"
#include "Mathematics/Vector3.h"
#include "Rendering/Core/HashUtils.h"
#include "Rendering/Core/PassPhase.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Core/ViewParamsLayout.h"
#include "Rendering/Passes/TonemapPass.h"
#include "Thumbnails/ModelThumbnailHandler.h"
#include "UI/UIManager.h"
#include "UI/UITextureSpace.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

namespace GameEngine::Editor
{
namespace
{
namespace RG = ::GameEngine::Rendering::RenderGraph;
using ::GameEngine::Rendering::MaterialKeyword;

// Stem of every per-instance resource name. Each atlas appends its own id, so
// two open material graphs address different textures and pass keys.
constexpr const char* kResourceStem = "editor_graph_preview_atlas";
constexpr const char* kDebugStem = "Editor.GraphPreviewAtlas";

/// Distinguishes the atlases of concurrently open material-graph panels.
uint32_t NextInstanceId()
{
    static std::atomic<uint32_t> s_next{1};
    return s_next.fetch_add(1, std::memory_order_relaxed);
}

// Base light world for the atlas views; each instance offsets by its own id.
// Never an ECS world id — the atlas submits its two preview lights directly.
constexpr uint64 kAtlasLightWorldBase = 0x4752'4150'4154'4C53ull; // "GRAPATLS"

// Camera framing, matching the material thumbnail preview's rig so the atlas
// look tracks the slot look it replaces.
constexpr float kFovRad = 45.0f * 3.1415926535f / 180.0f;
constexpr float kNearZ = 0.1f;
constexpr float kFarZ = 10.0f;
constexpr float kEyeX = 0.9f;
constexpr float kEyeY = 0.9f;
constexpr float kEyeZ = 1.8f;

// Preview key light direction at orbit yaw zero (ModelThumbnailHandler's rig).
constexpr float kKeyLightDirX = -0.267f;
constexpr float kKeyLightDirY = -0.802f;
constexpr float kKeyLightDirZ = -0.535f;

// Exposure of the IBL-off preview: the key/ambient rig is authored around
// reference white, so the tonemap runs at unity.
constexpr float kPreviewExposure = 1.0f;

// PC block of adapter_vertex.glsl's non-instanced path.
struct alignas(16) PreviewPushConstants
{
    float uM[16];
    float uN0[4];
    float uN1[4];
    float uN2[4];
    float uExtra[4];
};
static_assert(sizeof(PreviewPushConstants) == 128,
              "must match adapter_vertex.glsl's PC block");

/// Uniform sphere scale whose projected silhouette spans `fill` of the cell.
///
/// The frame's full height at distance d is 2*d*tan(fov/2) and the sphere's
/// apparent diameter is 2*d*tan(theta), so diameter/frameHeight is
/// tan(theta)/tan(fov/2) — the halves cancel. The silhouette's half-angle comes
/// from sin(theta) = r/d, not r/d directly: at this framing the tangent-vs-ratio
/// difference is several percent of the cell.
float SphereScaleForFill(float fill)
{
    const float camDist = std::sqrt(kEyeX * kEyeX + kEyeY * kEyeY + kEyeZ * kEyeZ);
    const float tanHalfFov = std::tan(kFovRad * 0.5f);
    const float tanTheta = std::clamp(fill, 0.05f, 0.99f) * tanHalfFov;
    const float sinTheta = tanTheta / std::sqrt(1.0f + tanTheta * tanTheta);
    // PrimitiveGenerator's sphere has radius 0.5.
    return (camDist * sinTheta) / 0.5f;
}

} // namespace

struct GraphPreviewAtlas::AtlasGeneration
{
    Rendering::IDevice* Device = nullptr;
    std::weak_ptr<std::atomic<bool>> ServiceLifetime;
    Rendering::TextureHandle Texture{};

    ~AtlasGeneration()
    {
        const auto alive = ServiceLifetime.lock();
        if (alive && alive->load(std::memory_order_acquire) && Device && Texture.IsValid())
            Device->DestroyTexture(Texture);
    }
};

struct GraphPreviewAtlas::Completion
{
    std::shared_ptr<AtlasGeneration> Generation;
    uint64_t Revision = 0;
    uint64_t Digest = 0;
    uint32_t Undrawn = 0;
};

struct GraphPreviewAtlas::State
{
    std::atomic<bool> Active{true};
    std::atomic<uint64_t> Revision{0};
    std::atomic<uint64_t> Serial{0};
    std::mutex CompletionMutex;
    std::shared_ptr<const Completion> Completed;

    std::shared_ptr<const Completion> ExchangeCompletion(std::shared_ptr<const Completion> completion)
    {
        std::lock_guard lock(CompletionMutex);
        Completed.swap(completion);
        return completion;
    }
};

struct GraphPreviewAtlas::FramePlan
{
    struct Cell
    {
        CellDraw Request;
        uint32_t X = 0, Y = 0;
        PreviewPushConstants Constants{};
    };
    struct PreparedCell
    {
        Engine::Renderer::PreparedDrawBindings Bindings;
        PreviewPushConstants Constants{};
        uint32_t X = 0, Y = 0;
        uint32_t MaterialIndex = 0;
        uint32_t ServedVersion = 0;
        bool HasServedVersion = false;
    };

    Engine::Renderer::RenderServices* Services = nullptr;
    std::weak_ptr<std::atomic<bool>> ServiceLifetime;
    std::shared_ptr<State> Owner;
    std::shared_ptr<AtlasGeneration> Generation;
    std::shared_ptr<Completion> Result;
    RG::RGFrameStamp Frame;
    RG::RGTexture Atlas;
    uint64_t Serial = 0, Revision = 0;
    Engine::Renderer::PassBindingContext Inputs;
    Rendering::MeshGPUEntryBindings Geometry;
    Rendering::MeshGPUHandle Sphere;
    uint64_t UploadSequence = 0;
    Rendering::VertexAttributeFlags VertexFlags{};
    Rendering::IndexType IndexType = Rendering::IndexType::Uint16;
    uint32_t IndexCount = 0, FirstIndex = 0;
    int32_t VertexOffset = 0;
    uint32_t Missing = 0;
    bool Ibl = false;
    bool GeometryRecorded = false, Acknowledged = false;
    std::vector<Cell> Cells;
    std::vector<PreparedCell> Prepared;

    bool IsCurrent() const
    {
        const auto alive = ServiceLifetime.lock();
        return alive && alive->load(std::memory_order_acquire) &&
            Owner->Active.load(std::memory_order_acquire) && !Acknowledged &&
            Owner->Serial.load(std::memory_order_acquire) == Serial &&
            Owner->Revision.load(std::memory_order_acquire) == Revision &&
            Services && Services->GetDevice() == Generation->Device &&
            Frame.Frame && Frame.IsFor(*Frame.Frame);
    }

    bool Prepare(RG::RGPrepareContext& context)
    {
        Prepared.clear();
        GeometryRecorded = false;
        if (!IsCurrent() || context.GetTexture(Atlas) != Generation->Texture ||
            !context.GetDevice()->IsTextureAlive(Generation->Texture))
            return false;
        Result->Undrawn = Missing;
        auto& meshes = Services->GetMeshGPURegistry();
        const auto* sphere = meshes.Find(Sphere);
        Rendering::MeshGPUEntryBindings current{};
        if (!sphere || sphere->uploadSeq != UploadSequence ||
            sphere->vertexFlags != VertexFlags || sphere->indexType != static_cast<uint32_t>(IndexType) ||
            !meshes.TryGetDrawableBindings(*sphere, current) ||
            current.coreVB != Geometry.coreVB || current.tangentVB != Geometry.tangentVB ||
            current.colorVB != Geometry.colorVB || current.uv1VB != Geometry.uv1VB ||
            current.indexBuffer != Geometry.indexBuffer || sphere->indexCount != IndexCount ||
            sphere->firstIndex != FirstIndex || sphere->vertexOffset != static_cast<uint32_t>(VertexOffset))
            return false;
        auto pass = Inputs;
        pass.FrameSlot = context.GetDevice()->GetFrameIndex();
        pass.Samples = context.GetCurrentSampleCount();
        auto& materials = Services->Materials();
        auto& registry = materials.Registry();
        for (const auto& cell : Cells)
        {
            const auto& request = cell.Request;
            if (registry.GetRegistrationGeneration(request.MaterialGuid) != request.Registration)
                return false;
            const auto* material = registry.Find(request.MaterialGuid);
            if (!material || material->GetVersion() != request.Version ||
                material->GetContentRevision() != request.ContentRevision)
                return false;
            Rendering::PipelineHandle pipeline{};
            const Engine::Renderer::PipelineVariantCache::VariantServeUnit* served = nullptr;
            if (Ibl)
            {
                const auto variant = materials.Variants().PrepareColorVariant(
                    *material, VertexFlags, Rendering::PrimitiveTopology::TriangleList,
                    MaterialKeyword::IBL, Rendering::FrontFace::CounterClockwise, context);
                pipeline = variant.first;
                served = variant.second;
            }
            else
                pipeline = context.PreparePipeline(material->GetGraphicsPipelineIdForFlags(
                    VertexFlags, context.GetDevice()));
            if (!pipeline.IsValid())
            {
                ++Result->Undrawn;
                continue;
            }
            const auto bindings = materials.Binder().PrepareMaterialForDraw(
                pass, *material, pipeline,
                served ? served->VariantMeta.get() : material->GetShaderMeta().get(), {}, 0,
                served ? std::span<const Rendering::DescriptorSetLayoutId>(served->SetLayouts)
                       : std::span<const Rendering::DescriptorSetLayoutId>{});
            if (!bindings.Pipeline.IsValid())
                return false;
            PreparedCell draw{bindings, cell.Constants, cell.X, cell.Y,
                              material->GetGpuSceneMaterialIndex(), served ? served->MaterialVersion : 0u,
                              served != nullptr};
            Prepared.push_back(draw);
            if (served && served->MaterialVersion != request.Version)
                ++Result->Undrawn;
        }
        return true;
    }

    void Record(RG::RGContext& context)
    {
        if (!context.Cmd || !IsCurrent())
            return;
        auto* commands = context.Cmd;
        auto& binder = Services->Materials().Binder();
        Engine::Renderer::PassBindingContext pass{};
        pass.Cmd = commands;
        const Rendering::BufferHandle vertices[] =
            {Geometry.coreVB, Geometry.tangentVB, Geometry.colorVB, Geometry.uv1VB};
        for (uint32_t slot = 0; slot < 4u; ++slot)
            if (vertices[slot].IsValid())
                commands->SetVertexBuffer(vertices[slot], slot);
        commands->SetIndexBuffer(Geometry.indexBuffer, IndexType);
        for (const auto& draw : Prepared)
        {
            constexpr uint32_t kCellPx = GraphPreviewAtlasLayout::kCellPx;
            commands->SetViewport(static_cast<float>(draw.X), static_cast<float>(draw.Y),
                                  static_cast<float>(kCellPx), static_cast<float>(kCellPx));
            commands->SetScissor(draw.X, draw.Y, kCellPx, kCellPx);
            const auto constants = std::span<const std::byte>(
                reinterpret_cast<const std::byte*>(&draw.Constants), sizeof(draw.Constants));
            if (!binder.BindPreparedForDraw(pass, draw.Bindings, constants).IsValid())
                continue;
            commands->DrawIndexed(IndexCount, 1, FirstIndex, VertexOffset, 0);
            if (draw.HasServedVersion)
                Services->Materials().Variants().RecordServedGeneration(
                    draw.MaterialIndex, Engine::Renderer::ServedPassClass::Color, draw.ServedVersion);
        }
        GeometryRecorded = true;
    }

    void Acknowledge(RG::RGContext& context)
    {
        if (!context.Cmd || !IsCurrent() || !GeometryRecorded ||
            context.GetTexture(Atlas) != Generation->Texture)
            return;
        Acknowledged = true;
        Owner->ExchangeCompletion(Result);
    }
};

GraphPreviewAtlas::~GraphPreviewAtlas()
{
    Shutdown();
}

void GraphPreviewAtlas::MarkDirty()
{
    m_Dirty = true;
    if (m_State)
        m_State->Revision.fetch_add(1, std::memory_order_acq_rel);
}

void GraphPreviewAtlas::DrainCompletion()
{
    if (!m_State)
        return;
    const auto result = m_State->ExchangeCompletion({});
    if (!result || result->Generation != m_Generation)
        return;
    m_AtlasInitialized = true;
    if (result->Revision != m_State->Revision.load(std::memory_order_acquire))
        return;
    m_ContentDigest = result->Digest;
    m_UndrawnCells = result->Undrawn;
    m_Dirty = false;
}

void GraphPreviewAtlas::Initialize(Engine::Renderer::RenderServices* services)
{
    m_Services = services;
    m_ServiceLifetime = services ? services->LifetimeToken() : std::weak_ptr<std::atomic<bool>>{};
    if (!m_State)
        m_State = std::make_shared<State>();
    if (!m_UiResourceName.empty())
        return;

    const uint32_t instanceId = NextInstanceId();
    const std::string suffix = std::to_string(instanceId);
    m_UiResourceName = std::string(kResourceStem) + "_" + suffix;
    const std::string debugStem = std::string(kDebugStem) + "." + suffix;
    m_AtlasResourceKey = debugStem;
    m_MsaaResourceKey = debugStem + ".ColorMSAA";
    m_DepthResourceKey = debugStem + ".Depth";
    m_HdrResourceKey = debugStem + ".HDR";
    m_PassName = debugStem;
    m_TonemapPassName = debugStem + ".Tonemap";
    m_LightWorldId = kAtlasLightWorldBase + instanceId;
}

void GraphPreviewAtlas::Shutdown()
{
    if (m_State)
        m_State->Active.store(false, std::memory_order_release);
    const auto alive = m_ServiceLifetime.lock();
    if (m_Services && alive && alive->load(std::memory_order_acquire) && m_Services->GetDevice())
    {
        if (m_ViewId != 0)
            m_Services->Views().ReleaseView(m_ViewId);
        if (m_CameraId != 0)
            m_Services->Views().ReleaseCamera(m_CameraId);
    }
    m_ViewId = 0;
    m_CameraId = 0;
    m_Generation.reset();
    m_State.reset();
    m_AtlasTexture = {};
    m_AtlasInitialized = false;
    m_UiBound = false;
    m_Dirty = true;
    m_ContentDigest = 0;
    m_UndrawnCells = 0;
    m_Requests.clear();
    m_Services = nullptr;
    m_ServiceLifetime.reset();
}

bool GraphPreviewAtlas::SetRequests(uint64_t windowId, std::vector<NodeRequest> requests)
{
    m_WindowId = windowId;
    if (m_Requests.size() == requests.size()
        && std::equal(m_Requests.begin(), m_Requests.end(), requests.begin(),
                      [](const NodeRequest& a, const NodeRequest& b)
                      { return a.NodeId == b.NodeId && a.MaterialGuid == b.MaterialGuid; }))
    {
        return false;
    }

    /* A re-sync rebuilds the request list from the model, which knows nothing
       about how far a cell has been spun. Carry each surviving cell's angle
       across or the preview snaps back to zero on every rebuild. */
    for (NodeRequest& incoming : requests)
    {
        for (const NodeRequest& previous : m_Requests)
        {
            if (previous.NodeId == incoming.NodeId)
            {
                incoming.YawRadians = previous.YawRadians;
                break;
            }
        }
    }

    m_Requests = std::move(requests);

    std::vector<std::string> keys;
    keys.reserve(m_Requests.size());
    for (const NodeRequest& r : m_Requests)
        keys.push_back(r.NodeId);
    m_Layout.Reconcile(keys);
    MarkDirty();
    return true;
}

bool GraphPreviewAtlas::TakeDeferredRebuild(bool gestureActive)
{
    if (!m_RebuildDeferred)
    {
        m_RebuildDeferredFrames = 0;
        return false;
    }
    ++m_RebuildDeferredFrames;
    if (gestureActive && m_RebuildDeferredFrames <= kMaxDeferredRebuildFrames)
        return false;
    m_RebuildDeferred = false;
    m_RebuildDeferredFrames = 0;
    return true;
}

void GraphPreviewAtlas::SetRequestYaw(const std::string& nodeId, float yawRadians)
{
    for (NodeRequest& request : m_Requests)
    {
        if (request.NodeId != nodeId)
            continue;
        if (request.YawRadians == yawRadians)
            return;
        request.YawRadians = yawRadians;
        MarkDirty();
        return;
    }
}

bool GraphPreviewAtlas::ClearRequests()
{
    if (m_Requests.empty())
        return false;
    m_Requests.clear();
    m_Layout.Reconcile({});
    MarkDirty();
    return true;
}

bool GraphPreviewAtlas::TryGetBinding(const std::string& nodeId,
                                      GraphNodePreviewBinding& out) const
{
    // Deliberately not gated on the atlas having rendered: the thumb binds the
    // moment its cell exists, and the UI simply paints nothing while the name is
    // unresolved. Gating here would bind "no preview" on the rebind that follows
    // a rebuild — before the atlas has ever run — and nothing would rebind after.
    uint32_t cell = 0;
    if (!m_Layout.TryGetCell(nodeId, cell))
        return false;
    const GraphPreviewAtlasLayout::CellRect rect = m_Layout.BackgroundRectForCell(cell);
    out.Resource = m_UiResourceName;
    out.SizeXPercent = rect.SizeXPercent;
    out.SizeYPercent = rect.SizeYPercent;
    out.PosXPercent = rect.PosXPercent;
    out.PosYPercent = rect.PosYPercent;
    return true;
}

uint64_t GraphPreviewAtlas::ComputeContentDigest() const
{
    uint64_t digest = Rendering::HashUtils::HashValue(
        0x9E37'79B9'7F4A'7C15ull, Engine::Renderer::Material::GetGlobalContentEpoch());
    auto& registry = m_Services->Materials().Registry();
    for (const NodeRequest& r : m_Requests)
    {
        const Engine::Renderer::Material* mat = registry.Find(r.MaterialGuid);
        digest = Rendering::HashUtils::HashValue(digest, registry.GetRegistrationGeneration(r.MaterialGuid));
        digest = Rendering::HashUtils::HashValue(digest, mat ? mat->GetVersion() : 0u);
        digest = Rendering::HashUtils::HashValue(
            digest, (mat && mat->GetGraphicsPipelineId().IsValid()) ? 1u : 0u);
    }
    digest = Rendering::HashUtils::HashValue(digest, m_IblEnabled && IblAvailable());
    digest = Rendering::HashUtils::HashValue(digest, m_Services->GetDefaultMSAASampleCount());
    digest = Rendering::HashUtils::HashValue(digest, static_cast<uint32_t>(m_Services->GetDepthFormat()));
    return digest;
}

void GraphPreviewAtlas::EnsureView()
{
    if (m_CameraId == 0)
        m_CameraId = m_Services->Views().AllocateCamera("Editor Graph Preview Atlas Camera");
    if (m_ViewId == 0)
    {
        m_ViewId = m_Services->Views().AllocateView("Editor Graph Preview Atlas View", m_CameraId,
                                                    Rendering::ViewPurpose::EditorPreview);
        m_Services->Views().SetViewWorldId(m_ViewId, m_LightWorldId);
    }
}

bool GraphPreviewAtlas::EnsureAtlasTexture(uint32_t width, uint32_t height)
{
    if (m_AtlasTexture.IsValid() && m_AtlasWidth == width && m_AtlasHeight == height)
        return true;

    auto* device = m_Services->GetDevice();
    if (!device || width == 0 || height == 0)
        return false;

    Rendering::TextureDesc td{};
    td.width = width;
    td.height = height;
    td.depth = 1;
    td.mipLevels = 1;
    td.arrayLayers = 1;
    td.sampleCount = 1;
    // Display-referred LINEAR, like the thumbnail slots: the editor's terminal
    // FinalSRGBEncode pass owns the only OETF, so an sRGB atlas would encode
    // twice. RGBA16F keeps the darks off the 8-bit linear banding floor.
    td.format = static_cast<uint32_t>(Rendering::TextureFormat::R16G16B16A16_FLOAT);
    td.usage = static_cast<uint32_t>(Rendering::TextureUsage::RenderTarget)
               | static_cast<uint32_t>(Rendering::TextureUsage::ShaderResource);
    td.debugName = m_AtlasResourceKey.c_str();
    auto generation = std::make_shared<AtlasGeneration>();
    generation->Device = device;
    generation->ServiceLifetime = m_ServiceLifetime;
    generation->Texture = device->CreateTexture(td);
    if (!generation->Texture.IsValid())
        return false;
    m_Generation = std::move(generation);
    m_AtlasTexture = m_Generation->Texture;
    MarkDirty();

    m_AtlasWidth = width;
    m_AtlasHeight = height;
    m_AtlasInitialized = false;
    m_UiBound = false;
    return true;
}

void GraphPreviewAtlas::SubmitPreviewLights()
{
    using ModelThumb = ModelThumbnailHandler;

    Components::Light key{};
    key.Type = Components::LightType::Directional;
    key.Color[0] = key.Color[1] = key.Color[2] = 1.0f;
    key.Intensity = std::clamp(ModelThumb::GetPreviewLightIntensity(), 0.25f, 4.0f);
    key.CastsShadows = false;

    Engine::Renderer::ExtractedLight keyOut{};
    keyOut.SortId = 1;
    keyOut.type = key.Type;
    Components::ResolveLightColorIntensity(key, keyOut.color, keyOut.intensity);
    keyOut.directionWS[0] = kKeyLightDirX;
    keyOut.directionWS[1] = kKeyLightDirY;
    keyOut.directionWS[2] = kKeyLightDirZ;
    keyOut.castsShadows = 0;
    m_Services->SubmitLight(m_LightWorldId, keyOut);

    Components::Light ambient{};
    ambient.Type = Components::LightType::Ambient;
    ambient.Color[0] = ambient.Color[1] = ambient.Color[2] = 1.0f;
    ambient.Intensity = std::max(0.0f, ModelThumb::GetPreviewAmbientIntensity());
    ambient.CastsShadows = false;

    Engine::Renderer::ExtractedLight ambientOut{};
    ambientOut.SortId = 2;
    ambientOut.type = ambient.Type;
    Components::ResolveLightColorIntensity(ambient, ambientOut.color, ambientOut.intensity);
    m_Services->SubmitLight(m_LightWorldId, ambientOut);

    m_Services->FinalizeWorldLights(m_LightWorldId);
    m_Services->WriteViewLightBuffer(m_ViewId);
}

Rendering::CameraData GraphPreviewAtlas::BuildCameraData() const
{
    using GameEngine::Mathematics::Matrix4x4;
    using GameEngine::Mathematics::Vector3;

    const Vector3 eye(kEyeX, kEyeY, kEyeZ);
    const Vector3 target(0.0f, 0.0f, 0.0f);
    const Vector3 up(0.0f, 1.0f, 0.0f);

    const Matrix4x4 viewM = GameEngine::Mathematics::MakeLookAtLH(eye, target, up);
    // Cells are square, so the projection is aspect 1 whatever the atlas shape.
    const Matrix4x4 projM =
        GameEngine::Mathematics::MakePerspectiveLH_ZO_ReverseZ(kFovRad, 1.0f, kNearZ, kFarZ);
    const Matrix4x4 viewProjM = projM * viewM;

    Rendering::CameraData cam{};
    const float* viewSrc = viewM.Data();
    const float* projSrc = projM.Data();
    const float* viewProjSrc = viewProjM.Data();
    for (int i = 0; i < 16; ++i)
    {
        cam.view[i] = viewSrc[i];
        cam.proj[i] = projSrc[i];
        cam.viewProj[i] = viewProjSrc[i];
        // Render origin is inactive here (sector 0), so the rebased matrices
        // equal the full-world ones and the shader takes the full-world path.
        cam.viewRel[i] = viewSrc[i];
        cam.viewProjRel[i] = viewProjSrc[i];
    }
    cam.cameraPos[0] = kEyeX;
    cam.cameraPos[1] = kEyeY;
    cam.cameraPos[2] = kEyeZ;
    cam.cameraPos[3] = 0.0f;
    return cam;
}

void GraphPreviewAtlas::SetIblEnabled(bool enabled)
{
    if (m_IblEnabled == enabled)
        return;
    m_IblEnabled = enabled;
    MarkDirty();
}

bool GraphPreviewAtlas::IblAvailable() const
{
    if (!m_Services)
        return false;
    const auto* ibl = m_Services->GetFeature<Engine::Renderer::ImageBasedLightingFeature>();
    return ibl != nullptr && ibl->IsInitialized();
}

void GraphPreviewAtlas::TickRG(uint64_t windowId, UIManager* ui, RG::RGFrame& frame)
{
    if (!m_Services || windowId == 0 || windowId != m_WindowId)
        return;

    DrainCompletion();

    if (m_Requests.empty())
    {
        if (m_UiBound && ui)
        {
            ui->RemoveExternalTexture(m_UiResourceName);
            m_UiBound = false;
        }
        return;
    }

    if (!EnsureAtlasTexture(m_Layout.WidthPx(), m_Layout.HeightPx()))
        return;
    EnsureView();
    if (m_ViewId == 0)
        return;

    // The atlas rides the graph even on frames it is not redrawn: the UI pass
    // resolves the name against this frame's publish, which is also the edge
    // that orders the atlas write before the UI sample.
    const RG::RGTexture atlas = frame.ImportExternalTexture(
        m_AtlasResourceKey.c_str(), m_AtlasTexture,
        m_AtlasInitialized ? Rendering::ResourceState::ShaderResource
                           : Rendering::ResourceState::Undefined,
        Rendering::TextureFormat::R16G16B16A16_FLOAT);
    if (!atlas.IsValid())
        return;

    // A planned producer may publish before it records: required preparation
    // gates the whole graph, and the UI read orders its sample after tonemap.
    // An idle graph publishes only previously recorded content.
    const auto publishToUi = [&](bool planned = false)
    {
        if (!ui || (!m_AtlasInitialized && !planned))
            return;
        ui->SetExternalTextureRG(m_UiResourceName, m_AtlasWidth, m_AtlasHeight,
                                 UI::UITextureSpace::DisplayLinearSdr(),
                                 Rendering::TextureFormat::R16G16B16A16_FLOAT);
        ui->PublishExternalTextureRG(m_UiResourceName, frame, atlas);
        m_UiBound = true;
    };

    const uint64_t contentDigest = ComputeContentDigest();
    const bool contentMoved = contentDigest != m_ContentDigest;
    if (!m_Dirty && !contentMoved && m_UndrawnCells == 0)
    {
        publishToUi();
        return;
    }

    auto& meshRegistry = m_Services->GetMeshGPURegistry();
    const Rendering::MeshGPUEntry* sphere = meshRegistry.FindByKey(
        {Engine::Renderer::PrimitiveGenerator::SphereGuid(), 0});
    Rendering::MeshGPUEntryBindings sphereBindings{};
    if (!sphere || sphere->indexCount == 0
        || !meshRegistry.TryGetDrawableBindings(*sphere, sphereBindings))
    {
        return;
    }

    // Missing materials remain pending. Preparation selects each ready variant
    // against this pass's formats and copies its bindings into the frame plan.
    std::vector<CellDraw> draws;
    draws.reserve(m_Requests.size());
    uint32_t undrawn = 0;
    auto& registry = m_Services->Materials().Registry();
    for (const NodeRequest& request : m_Requests)
    {
        uint32_t cell = 0;
        if (!m_Layout.TryGetCell(request.NodeId, cell))
            continue;
        const Engine::Renderer::Material* mat = registry.Find(request.MaterialGuid);
        if (!mat || !mat->GetGraphicsPipelineId().IsValid() || !mat->GetShaderMeta())
        {
            ++undrawn;
            continue;
        }
        draws.push_back(CellDraw{cell, request.MaterialGuid,
            registry.GetRegistrationGeneration(request.MaterialGuid), mat->GetVersion(),
            mat->GetContentRevision(), request.YawRadians, request.FillFraction});
    }
    if (draws.empty())
    {
        m_UndrawnCells = undrawn;
        publishToUi();
        return;
    }

    SubmitPreviewLights();

    // Same sample count as the thumbnail slots this replaces: a 256 px sphere
    // silhouette is all edge, so dropping MSAA here would be a visible
    // regression against the previews it takes over.
    const uint32_t samples = m_Services->GetDefaultMSAASampleCount();
    const bool msaa = samples > 1u;

    Rendering::TextureDesc td{};
    td.width = m_AtlasWidth;
    td.height = m_AtlasHeight;
    td.depth = 1;
    td.mipLevels = 1;
    td.arrayLayers = 1;

    RG::RGTexture msaaColor{};
    if (msaa)
    {
        td.sampleCount = samples;
        // Must match the resolve target's format, or the resolve is invalid.
        td.format = static_cast<uint32_t>(Rendering::TextureFormat::R16G16B16A16_FLOAT);
        td.usage = static_cast<uint32_t>(Rendering::TextureUsage::RenderTarget);
        td.debugName = m_MsaaResourceKey.c_str();
        msaaColor = frame.ImportPersistentTexture(m_MsaaResourceKey.c_str(), td);
    }

    td.sampleCount = msaa ? samples : 1u;
    td.format = static_cast<uint32_t>(m_Services->GetDepthFormat());
    td.usage = static_cast<uint32_t>(Rendering::TextureUsage::DepthStencil)
               | static_cast<uint32_t>(Rendering::TextureUsage::ShaderResource);
    td.debugName = m_DepthResourceKey.c_str();
    const RG::RGTexture depth =
        frame.ImportPersistentTexture(m_DepthResourceKey.c_str(), td);

    td.sampleCount = 1;
    td.format = static_cast<uint32_t>(Rendering::TextureFormat::R16G16B16A16_FLOAT);
    td.usage = static_cast<uint32_t>(Rendering::TextureUsage::RenderTarget)
               | static_cast<uint32_t>(Rendering::TextureUsage::ShaderResource);
    td.debugName = m_HdrResourceKey.c_str();
    const RG::RGTexture hdr = frame.ImportPersistentTexture(m_HdrResourceKey.c_str(), td);

    if (!depth.IsValid() || !hdr.IsValid() || (msaa && !msaaColor.IsValid()))
        return;

    // Recording the IBL keyword without the feature's cubes would skip every
    // draw, and the undrawn-cell retry would re-declare the pass every frame
    // with nothing ever landing. Fall back to the single-light rig instead.
    const bool useIbl = m_IblEnabled && IblAvailable();

    // Per-pass resources the base (keyword-less) material variant reflects:
    // camera, per-view light UBO, view params, and the shared material SSBO.
    // Textures resolve through the global bindless set, which the binder
    // short-circuits.
    Engine::Renderer::ResolvedPassResources resources;
    {
        const auto camUpload = frame.AllocUpload<Rendering::CameraData>();
        if (!camUpload.Valid())
            return;
        *camUpload.Ptr = BuildCameraData();
        resources.Buffers.push_back(
            {HashStringId("Cam"), camUpload.Buffer, camUpload.Offset, sizeof(Rendering::CameraData)});

        const auto viewParamsUpload = frame.AllocUpload<Rendering::ViewParamsUBO>();
        if (!viewParamsUpload.Valid())
            return;
        Rendering::ViewParamsUBO vp{};
        const Rendering::CameraData cam = BuildCameraData();
        std::memcpy(vp.ge_view, cam.view, sizeof(vp.ge_view));
        std::memcpy(vp.ge_viewProj, cam.viewProj, sizeof(vp.ge_viewProj));
        std::memcpy(vp.ge_proj, cam.proj, sizeof(vp.ge_proj));
        std::memcpy(vp.ge_prevViewProj, cam.viewProj, sizeof(vp.ge_prevViewProj));
        vp.ge_nearFar[0] = kNearZ;
        vp.ge_nearFar[1] = kFarZ;
        vp.ge_cameraPosWS[0] = kEyeX;
        vp.ge_cameraPosWS[1] = kEyeY;
        vp.ge_cameraPosWS[2] = kEyeZ;
        vp.ge_screenSize[0] = static_cast<float>(GraphPreviewAtlasLayout::kCellPx);
        vp.ge_screenSize[1] = static_cast<float>(GraphPreviewAtlasLayout::kCellPx);
        vp.ge_screenSize[2] = 1.0f / vp.ge_screenSize[0];
        vp.ge_screenSize[3] = 1.0f / vp.ge_screenSize[1];
        *viewParamsUpload.Ptr = vp;
        resources.Buffers.push_back({HashStringId("ViewParams"), viewParamsUpload.Buffer,
                                     viewParamsUpload.Offset, sizeof(Rendering::ViewParamsUBO)});

        if (const auto* perView = m_Services->Views().FindPerView(m_ViewId))
        {
            const uint32_t frameSlot = m_Services->GetDevice()->GetFrameIndex();
            const auto lightBuffer = perView->LightBuffers[frameSlot];
            if (lightBuffer.IsValid())
            {
                resources.Buffers.push_back({HashStringId("LightUBO"), lightBuffer, 0, 0});
                resources.Buffers.push_back({HashStringId("Light"), lightBuffer, 0, 0});
            }
        }

        const auto materialParams = m_Services->Materials().PackedMaterialParams();
        if (!materialParams.Buffer.IsValid())
            return;
        resources.Buffers.push_back({HashStringId("MaterialParams"), materialParams.Buffer,
                                     materialParams.Offset, materialParams.Size});

        /* IBL: the environment cubes, BRDF LUT and EnvData UBO are
           engine-shared and view-independent — bound exactly the way the
           world pass binds them (RenderServicesWorldPass, IBL keyword block). */
        if (useIbl)
        {
            auto* ibl = m_Services->GetFeature<Engine::Renderer::ImageBasedLightingFeature>();
            const Rendering::BufferHandle envData = ibl->UploadEnvData(m_Services->GetDevice());
            resources.Textures.push_back({HashStringId("ge_irradianceCube"),
                                          ibl->GetIrradianceCube(), ibl->GetCubeSampler()});
            resources.Textures.push_back({HashStringId("ge_prefilterCube"),
                                          ibl->GetPrefilterCube(), ibl->GetCubeSampler()});
            resources.Textures.push_back(
                {HashStringId("ge_brdfLUT"), ibl->GetBrdfLut(), ibl->GetLutSampler()});
            if (envData.IsValid())
                resources.Buffers.push_back({HashStringId("Env"), envData, 0, 0});
        }
    }

    auto* plan = frame.AllocatePassData<FramePlan>();
    plan->Services = m_Services;
    plan->ServiceLifetime = m_ServiceLifetime;
    plan->Owner = m_State;
    plan->Generation = m_Generation;
    plan->Frame.Stamp(frame);
    plan->Atlas = atlas;
    plan->Serial = m_State->Serial.fetch_add(1, std::memory_order_acq_rel) + 1u;
    plan->Revision = m_State->Revision.load(std::memory_order_acquire);
    plan->Result = std::make_shared<Completion>();
    plan->Result->Generation = m_Generation;
    plan->Result->Revision = plan->Revision;
    plan->Result->Digest = contentDigest;
    plan->Inputs.View = m_ViewId;
    plan->Inputs.PassKeywords = useIbl ? MaterialKeyword::IBL : MaterialKeyword::None;
    plan->Inputs.PassResources = std::move(resources);
    plan->Geometry = sphereBindings;
    plan->Sphere = meshRegistry.FindHandle({Engine::Renderer::PrimitiveGenerator::SphereGuid(), 0});
    plan->UploadSequence = sphere->uploadSeq;
    plan->VertexFlags = sphere->vertexFlags;
    plan->IndexType = static_cast<Rendering::IndexType>(sphere->indexType);
    plan->IndexCount = sphere->indexCount;
    plan->FirstIndex = sphere->firstIndex;
    plan->VertexOffset = static_cast<int32_t>(sphere->vertexOffset);
    plan->Missing = undrawn;
    plan->Ibl = useIbl;
    plan->Cells.reserve(draws.size());
    for (const auto& draw : draws)
    {
        FramePlan::Cell cell;
        cell.Request = draw;
        m_Layout.CellOrigin(draw.Cell, cell.X, cell.Y);
        const float scale = SphereScaleForFill(draw.FillFraction);
        const float cy = std::cos(draw.YawRadians), sy = std::sin(draw.YawRadians);
        auto& pc = cell.Constants;
        pc.uM[0] = scale * cy;
        pc.uM[2] = -scale * sy;
        pc.uM[8] = scale * sy;
        pc.uM[10] = scale * cy;
        pc.uM[5] = scale;
        pc.uM[15] = 1.0f;
        pc.uN0[0] = cy;
        pc.uN0[2] = -sy;
        pc.uN1[1] = 1.0f;
        pc.uN2[0] = sy;
        pc.uN2[2] = cy;
        const auto* material = registry.Find(draw.MaterialGuid);
        const uint32_t materialIndex = material->GetGpuSceneMaterialIndex();
        std::memcpy(&pc.uExtra[0], &materialIndex, sizeof(materialIndex));
        plan->Cells.push_back(cell);
    }
    frame.AddPass(
        m_PassName.c_str(), Rendering::PassPhase::kWorldRender,
        [&](RG::RGPassBuilder& p)
        {
            RG::RGAttachmentOps color{};
            color.Load = RG::RGLoadOp::Clear;
            color.Store = RG::RGStoreOp::Store;
            if (msaa)
                p.AttachColorResolve(0, msaaColor, hdr, color);
            else
                p.AttachColor(0, hdr, color);

            RG::RGAttachmentOps depthOps{};
            depthOps.Load = RG::RGLoadOp::Clear;
            depthOps.Store = RG::RGStoreOp::Store;
            // Reverse-Z: the far value is 0.
            depthOps.Clear.Depth = 0.0f;
            p.AttachDepth(depth, depthOps, RG::RGDepthAccess::ReadWrite);
            p.Prepare([plan](RG::RGPrepareContext& context) { return plan->Prepare(context); });
        },
        [plan](RG::RGContext& context) { plan->Record(context); });

    Rendering::Passes::TonemapParams tmParams{};
    tmParams.preserveAlpha = 1;
    tmParams.exposure = kPreviewExposure;
    const RG::RGPass tonemap = Rendering::Passes::AddTonemapPassRG(
        frame, hdr, atlas, tmParams, m_TonemapPassName.c_str());
    if (!tonemap.IsValid())
        return;

    // Required tonemap preparation guarantees its fullscreen draw before this acknowledgment.
    const auto acknowledgment = frame.AddPass(
        (m_PassName + ".Acknowledge").c_str(), Rendering::PassPhase::kFinalize,
        [plan, atlas](RG::RGPassBuilder& pass)
        {
            pass.Read(atlas, RG::RGTextureRead::Sampled);
            pass.PreventCulling();
            pass.Prepare([plan](RG::RGPrepareContext&) { return plan->IsCurrent(); });
        },
        [plan](RG::RGContext& context) { plan->Acknowledge(context); });
    frame.AddOrderingEdge(tonemap, acknowledgment);
    frame.MarkOutput(atlas, RG::RGImageLayout::ShaderReadOnly);
    publishToUi(true);
}

} // namespace GameEngine::Editor
