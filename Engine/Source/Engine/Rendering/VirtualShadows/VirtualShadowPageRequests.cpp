#include "Engine/Rendering/VirtualShadows/VirtualShadowPageRequests.h"

#include "../RenderServicesDetail.h"

#include "Engine/Rendering/FeatureDeclareContext.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"
#include "Engine/Rendering/RenderOrigin.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/ShadowMapRenderFeature.h"
#include "Engine/Rendering/VirtualShadows/VirtualShadowFeedback.h"
#include "Engine/Rendering/VirtualShadows/VirtualShadowLookup.h"
#include "Engine/Rendering/VirtualShadows/VirtualShadowPageMark.h"
#include "Engine/Rendering/VirtualShadows/VirtualShadowRenderFeature.h"
#include "Engine/Rendering/VirtualShadows/VirtualShadowViewInputs.h"

#include "Logger/Logger.h"
#include "Mathematics/MatrixOps.h"
#include "Platform/Environment.h"
#include "Rendering/Core/NamedDescriptorWriter.h"
#include "Rendering/Core/PassPhase.h"
#include "Rendering/Core/RecomputeElision.h"
#include "Rendering/Materials/MaterialHelper.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <vector>

namespace GameEngine::Engine::Renderer
{
using namespace ::GameEngine::Rendering;
using Mathematics::Matrix4x4;
using Mathematics::Vector3;
using Mathematics::Vector3Int;
using Mathematics::WorldPosition;

namespace
{
constexpr uint32_t kWorkgroupSide = 8;
// The spot and point filter radius of clustered_lighting.glsl, in face UV.
constexpr float kLocalFilterRadiusUV = 1.5f / 1024.0f;
constexpr float kLocalNormalBias = 0.02f;
constexpr float kMaxDirectionalFilterRadiusUV = 0.25f;
constexpr float kDirectionalFilterTexels = 1.5f;

uint32_t VirtualSide(uint32_t resolution)
{
    const uint32_t pages = std::max(resolution / kVirtualShadowPageTexels, 1u);
    return std::min(std::bit_floor(pages), kVirtualLocalShadowPagesPerAxis);
}

// One light projection of a view: the mark row, the feedback address range and the raster camera.
struct ProjectionSet
{
    std::vector<VirtualShadowMarkProjectionGPU> Rows;
    std::vector<CameraData> Cameras;
    VirtualShadowFeedbackLayout Layout;
    uint32_t PageCount = 0;
};

struct ProjectionInput
{
    VirtualShadowPage Origin;
    Matrix4x4 WorldToLight;
    Matrix4x4 RelativeToLight;
    Matrix4x4 LightView;
    Matrix4x4 LightProjection;
    WorldPosition Position;
    float FarPlane = 0.0f;
    uint32_t PagesPerAxis = 0;
    uint32_t LastMip = 0;
    float NormalBias = kLocalNormalBias;
    float FilterRadiusUV = kLocalFilterRadiusUV;
    const VirtualShadowDirectionalGrid* Grid = nullptr;
};

void AddProjection(ProjectionSet& set, const ProjectionInput& input, const CameraData& camera,
                   const Matrix4x4& inverseViewProjection)
{
    VirtualShadowMarkProjectionGPU row{};
    const Matrix4x4 transform = input.RelativeToLight * inverseViewProjection;
    row.ViewClipToLightClip = transform;
    row.FirstPage = set.PageCount;
    row.PagesPerAxis = input.PagesPerAxis;
    row.LastMip = input.LastMip;
    row.FilterRadiusUV = input.FilterRadiusUV;
    const float* relative = input.RelativeToLight.Data();
    for (uint32_t axis = 0; axis < 4; ++axis)
        row.NormalBiasClipBounds[axis] = input.NormalBias *
            std::sqrt(relative[axis] * relative[axis] + relative[axis + 4] * relative[axis + 4] +
                      relative[axis + 8] * relative[axis + 8]);
    set.Rows.push_back(row);

    VirtualShadowFeedbackProjection address{input.Origin, input.WorldToLight, set.PageCount,
                                            input.PagesPerAxis, input.LastMip};
    if (input.Grid)
        address.DirectionalGrid = *input.Grid;
    else
        address.LocalPosition = input.Position;
    set.Layout.Projections.push_back(std::move(address));

    CameraData lightCamera{};
    lightCamera.view = input.LightView;
    lightCamera.viewRel = input.LightView;
    lightCamera.proj = input.LightProjection;
    lightCamera.viewProj = input.WorldToLight;
    lightCamera.viewProjRel = input.RelativeToLight;
    std::copy_n(camera.renderOriginSector, 4, lightCamera.renderOriginSector);
    const Vector3Int sector = CameraRenderOriginSector(camera);
    if (!input.Grid && (sector.X != 0 || sector.Y != 0 || sector.Z != 0))
    {
        const auto translation = ComputeRebasedViewTranslation(input.LightView, input.Position, sector);
        lightCamera.viewRel[3][0] = static_cast<float>(translation.X);
        lightCamera.viewRel[3][1] = static_cast<float>(translation.Y);
        lightCamera.viewRel[3][2] = static_cast<float>(translation.Z);
    }
    lightCamera.cameraPos = Mathematics::Vector4{static_cast<float>(input.Position.X),
        static_cast<float>(input.Position.Y), static_cast<float>(input.Position.Z), input.FarPlane};
    set.Cameras.push_back(lightCamera);

    for (uint32_t mip = 0; mip <= input.LastMip; ++mip)
    {
        const uint32_t mipSide = input.PagesPerAxis >> mip;
        set.PageCount += mipSide * mipSide;
    }
}

// The row the lighting shader reads for one projection.
VirtualShadowLookupProjectionGPU MakeLookupRow(const VirtualShadowMarkProjectionGPU& mark,
    const VirtualShadowFeedbackProjection& address, const CameraData& lightCamera,
    std::span<const VirtualShadowPageWork> work)
{
    VirtualShadowLookupProjectionGPU row{};
    row.RelativeToLightClip = lightCamera.viewProjRel;
    row.FirstPage = mark.FirstPage;
    row.PagesPerAxis = mark.PagesPerAxis;
    row.LastMip = mark.LastMip;
    if (address.DirectionalGrid)
    {
        row.LightPositionRelAndFar[0] =
            static_cast<float>(address.DirectionalGrid->PageWorldSize / kVirtualShadowPageTexels);
        row.LightPositionRelAndFar[1] =
            static_cast<float>(address.DirectionalGrid->DepthStep * kVirtualShadowDirectionalDepthBands);
        row.LightPositionRelAndFar[2] = VirtualShadowResidentRadius(work, kVirtualShadowPhysicalTiles, address);
        return row;
    }
    const WorldPosition position = address.LocalPosition.value_or(WorldPosition{});
    const double sectorSize = Mathematics::kWorldSectorSize;
    row.LightPositionRelAndFar[0] = static_cast<float>(position.X - lightCamera.renderOriginSector[0] * sectorSize);
    row.LightPositionRelAndFar[1] = static_cast<float>(position.Y - lightCamera.renderOriginSector[1] * sectorSize);
    row.LightPositionRelAndFar[2] = static_cast<float>(position.Z - lightCamera.renderOriginSector[2] * sectorSize);
    row.LightPositionRelAndFar[3] = lightCamera.cameraPos.w;
    return row;
}
} // namespace

VirtualShadowPageRequests::~VirtualShadowPageRequests()
{
    if (m_Device && m_Sampler.IsValid())
        m_Device->DestroySampler(m_Sampler);
}

bool VirtualShadowPageRequests::EnsurePipeline(IDevice& device)
{
    if (m_Device)
        return m_Pipeline.IsValid() && m_Sampler.IsValid();
    m_Device = &device;
    ShaderPackage package{};
    std::string error;
    if (!LoadShaderPkg("Shaders/virtual_shadow_mark.shaderpkg", device.PreferredShaderSource(), package, &error))
    {
        LOG_WARNING("Virtual shadows are off: the page mark shader did not load ({}). Rebuild the shaders.", error);
        return false;
    }
    const auto compute = package.stageBytes.find("cs");
    if (compute == package.stageBytes.end() || compute->second.empty())
        return false;

    m_Meta = std::move(package.meta);
    ComputePipelineDesc descriptor{};
    descriptor.ComputeShader = std::make_shared<const std::vector<uint8_t>>(std::move(compute->second));
    descriptor.DebugName = "VirtualShadowPageRequests";
    const auto saveLayout = [this](uint32_t set, DescriptorSetLayoutDesc& layout)
    {
        if (set == 0)
            m_Layout = layout;
    };
    if (!MaterialHelper::ApplyShaderMetaToComputeDesc(device, m_Meta, descriptor,
            MaterialBuilder::MergeMode::Auto, {true, 128}, saveLayout, &error))
    {
        LOG_WARNING("Virtual shadows are off: the page mark pipeline is not valid ({}).", error);
        return false;
    }
    m_Pipeline = device.InternComputePipeline(std::move(descriptor));
    m_Sampler = device.CreateSampler(SamplerDesc::PointClamp("VirtualShadowPageRequests.Depth"));
    return m_Pipeline.IsValid() && m_Sampler.IsValid();
}

void VirtualShadowPageRequests::DeclareForView(Pipeline::ViewDeclare& d, VirtualShadowRenderFeature& feature,
    const FeatureDeclareContext& shadows, SamplerHandle compareSampler, const Directional& directional)
{
    const CascadeFrameData* cascades = directional.Cascades;
    auto* device = d.Services.GetDevice();
    const auto depth = d.ViewDepthResolved;
    if (!feature.IsEnabled() || !compareSampler.IsValid() || !device || d.Services.GetProfile().IsCompat() ||
        device->GetAPI() == GraphicsAPI::WebGPU || !depth.IsValid() || d.View.cameraId == 0 ||
        (d.View.purpose != ViewPurpose::Game && d.View.purpose != ViewPurpose::EditorScene))
        return;

    const auto& depthDescriptor = d.Frame.Graph().ResourceDesc(depth.Id);
    if (depthDescriptor.SampleCount != 1 || depthDescriptor.Width == 0 || depthDescriptor.Height == 0)
        return;
    const ViewLetterbox box = d.Services.Views().GetViewLetterbox(d.View.id);
    const uint32_t x = box.active ? box.x : 0;
    const uint32_t y = box.active ? box.y : 0;
    const uint32_t width = box.active ? box.width : depthDescriptor.Width;
    const uint32_t height = box.active ? box.height : depthDescriptor.Height;
    if (width == 0 || height == 0 || x > depthDescriptor.Width || y > depthDescriptor.Height ||
        width > depthDescriptor.Width - x || height > depthDescriptor.Height - y)
        return;

    // The depth prepass uses the jittered camera, so the mark pass uses it too.
    const CameraData camera = d.Services.Views().ResolveJitteredCameraData(
        d.View.id, d.Frame.FrameIndex(), width, height);
    if (camera.renderOriginSector[3] == 0 || (camera.proj[0][0] == 0.0f && camera.proj[1][1] == 0.0f))
        return;
    const Matrix4x4 inverseViewProjection = Mathematics::Inverse(camera.viewProjRel);
    const Vector3Int sector = CameraRenderOriginSector(camera);
    const VirtualShadowOptions& options = feature.GetOptions();

    const auto lights = d.Services.GetWorldLights(d.View.worldId);
    ProjectionSet set;
    set.Layout.World = d.View.worldId;
    set.Layout.RenderLayerMask = d.View.ActiveRenderLayerMask().Bits;
    VirtualShadowLookupHeaderGPU lookup{};
    lookup.DirectionalRows.fill(kUnmappedShadowPage);
    lookup.LocalRows.fill(kUnmappedShadowPage);
    static_assert(kVirtualShadowDirectionalRows == kMaxShadowCascades);

    if (const auto* sun = SelectPrimaryDirectional(lights); sun && cascades && shadows.ViewNeedsCascades)
    {
        const uint32_t side = VirtualSide(options.DirectionalResolution);
        const Vector3 direction{sun->directionWS[0], sun->directionWS[1], sun->directionWS[2]};
        const Matrix4x4 lightRotation = ShadowMapRenderFeature::CascadeLightRotation(direction.Normalize());
        // The scene fit moves with the camera. The authored distance does not.
        const auto& world = d.Services.GetWorldShadowSettings(d.View.worldId);
        const float shadowDistance = world.HasOverride ? world.MaxShadowDistance : directional.AuthoredShadowDistance;
        const float worldPerTexel = directional.CascadeResolution != 0
            ? 2.0f * cascades->OrthoHalfExtent[0] / static_cast<float>(directional.CascadeResolution) : 0.0f;
        for (uint32_t level = 0; level < cascades->NumCascades; ++level)
        {
            const auto window = BuildVirtualShadowDirectionalWindow(sun->SortId, level, cascades->NumCascades,
                side, shadowDistance, lightRotation, CameraEye(camera), sector);
            if (!window)
                continue;
            lookup.DirectionalRows[level] = static_cast<uint32_t>(set.Rows.size());
            ProjectionInput input{};
            input.Origin = window->Origin;
            input.WorldToLight = window->WorldToLight;
            input.RelativeToLight = window->RelativeToLight;
            input.LightView = Matrix4x4{};
            input.LightProjection = Matrix4x4{};
            input.PagesPerAxis = side;
            input.NormalBias = std::max(cascades->NormalBias, 0.0f);
            input.FilterRadiusUV = std::clamp(kDirectionalFilterTexels * worldPerTexel * directional.Softness /
                static_cast<float>(window->Grid.PageWorldSize * side), 0.0f, kMaxDirectionalFilterRadiusUV);
            input.Grid = &window->Grid;
            AddProjection(set, input, camera, inverseViewProjection);
        }
    }

    std::vector<const ExtractedLight*> clusteredLights;
    clusteredLights.reserve(lights.size());
    for (const auto& light : lights)
        if (light.type != Components::LightType::Directional && light.type != Components::LightType::Ambient)
            clusteredLights.push_back(&light);

    const uint32_t localSide = VirtualSide(options.LocalResolution);
    const uint32_t localLastMip = static_cast<uint32_t>(std::bit_width(localSide)) - 1u;
    const size_t localRows = std::min(clusteredLights.size(), lookup.LocalRows.size());
    if (shadows.SpotShadow)
    {
        uint32_t spotCount = 0;
        for (uint32_t index = 0; index < localRows && spotCount < kVirtualShadowMaxSpotProjections; ++index)
        {
            const auto& light = *clusteredLights[index];
            if (light.type != Components::LightType::Spot || light.castsLight == 0 || light.castsShadows == 0)
                continue;
            const auto spot = BuildSpotShadowFrameInfo(light, index);
            lookup.LocalRows[index] = static_cast<uint32_t>(set.Rows.size());
            ProjectionInput input{};
            input.Origin = {light.SortId, VirtualShadowLight::Spot};
            input.WorldToLight = spot.lightVP;
            input.RelativeToLight =
                ComputeRebasedLightVP(spot.lightView, spot.lightProj, spot.lightVP, spot.position, sector);
            input.LightView = spot.lightView;
            input.LightProjection = spot.lightProj;
            input.Position = spot.position;
            input.FarPlane = spot.farPlane;
            input.PagesPerAxis = localSide;
            input.LastMip = localLastMip;
            AddProjection(set, input, camera, inverseViewProjection);
            ++spotCount;
        }
    }
    for (const auto& point : shadows.PointShadows)
    {
        if (point.packedLightIndex >= localRows)
            continue;
        const auto& light = *clusteredLights[point.packedLightIndex];
        lookup.LocalRows[point.packedLightIndex] = static_cast<uint32_t>(set.Rows.size());
        // A filter footprint can cross a cube edge, so each face has pages.
        for (uint32_t face = 0; face < kPointShadowFaceCount; ++face)
        {
            ProjectionInput input{};
            input.Origin = {light.SortId, VirtualShadowLight::Point, static_cast<uint8_t>(face)};
            input.WorldToLight = point.lightVP[face];
            input.RelativeToLight = ComputeRebasedLightVP(point.lightView[face], point.lightProj,
                                                          point.lightVP[face], point.position, sector);
            input.LightView = point.lightView[face];
            input.LightProjection = point.lightProj;
            input.Position = point.position;
            input.FarPlane = point.farPlane;
            input.PagesPerAxis = localSide;
            input.LastMip = localLastMip;
            // The reprojection across a cube edge moves both UV axes of the adjacent face.
            input.FilterRadiusUV = 2.0f * kLocalFilterRadiusUV / (1.0f - 2.0f * kLocalFilterRadiusUV);
            AddProjection(set, input, camera, inverseViewProjection);
        }
    }
    if (set.Rows.empty() || !EnsurePipeline(*device))
        return;

    const size_t projectionBytes = set.Rows.size() * sizeof(VirtualShadowMarkProjectionGPU);
    const auto projectionUpload = d.Frame.AllocUpload(projectionBytes);
    const auto params = d.Frame.AllocUpload<VirtualShadowMarkParamsGPU>();
    if (!projectionUpload.Valid() || !params.Valid())
        return;
    const uint32_t wordCount = (set.PageCount + 31) / 32;
    set.Layout.WordCount = wordCount;
    const uint32_t projectionCount = static_cast<uint32_t>(set.Rows.size());
    std::memcpy(projectionUpload.Ptr, set.Rows.data(), projectionBytes);
    *params.Ptr = {width, height, projectionCount, wordCount, x, y, {}, {}};
    for (uint32_t axis = 0; axis < 4; ++axis)
        params.Ptr->CameraInverseW[axis] = inverseViewProjection[axis][3];

    BufferDesc descriptor{};
    descriptor.size = wordCount * sizeof(uint32_t);
    descriptor.stride = sizeof(uint32_t);
    descriptor.usage = static_cast<uint32_t>(BufferUsage::Storage | BufferUsage::TransferDst | BufferUsage::TransferSrc);
    descriptor.memoryUsage = BufferMemoryUsage::DeviceLocal;
    descriptor.debugName = "VirtualShadowPageRequests";
    bool needsInitialization = false;
    const auto requests = d.Frame.ImportPersistentBuffer(
        d.PassName("VirtualShadowPageRequests").c_str(), descriptor, &needsInitialization);
    if (!requests.IsValid())
        return;
    d.Frame.MarkOutput(requests);

    const auto& idle = d.Services.GetIdleElisionFrameState();
    static const bool reuseEnabled = Platform::EnvironmentSwitchEnabled("GE_VIRTUAL_SHADOW_REQUEST_REUSE", false);
    const bool allowReuse = reuseEnabled && idle.AllowCluster;
    ElisionInputBlob inputs;
    ElisionInputBlob domain;
    if (allowReuse)
    {
        // Each jittered phase reuses only its own submitted mask.
        inputs.AppendBytes(&camera, sizeof(camera));
        inputs.AppendBytes(set.Rows.data(), projectionBytes);
        inputs.AppendBytes(params.Ptr, sizeof(VirtualShadowMarkParamsGPU));
        const CameraData unjittered = d.Services.Views().ResolveCameraData(d.View.id);
        domain.AppendBytes(&unjittered, sizeof(unjittered));
        domain.Append(params.Ptr->Width);
        domain.Append(params.Ptr->Height);
        domain.Append(params.Ptr->ProjectionCount);
        domain.Append(params.Ptr->RequestWordCount);
        domain.Append(params.Ptr->ViewportX);
        domain.Append(params.Ptr->ViewportY);
        for (size_t index = 0; index < set.Rows.size(); ++index)
        {
            const auto& row = set.Rows[index];
            const auto& projection = set.Layout.Projections[index];
            // The address domain stays fixed while masks are ORed together.
            domain.Append(projection.BasePage.LightId);
            domain.Append(projection.BasePage.Family);
            domain.Append(projection.BasePage.Projection);
            domain.Append(projection.BasePage.Mip);
            domain.Append(projection.BasePage.X);
            domain.Append(projection.BasePage.Y);
            domain.AppendBytes(projection.WorldToLight.Data(), sizeof(float) * 16);
            domain.Append(row.FirstPage);
            domain.Append(row.PagesPerAxis);
            domain.Append(row.LastMip);
            domain.Append(row.FilterRadiusUV);
            domain.AppendBytes(row.NormalBiasClipBounds.v, sizeof(row.NormalBiasClipBounds.v));
        }
        for (ElisionInputBlob* blob : {&inputs, &domain})
        {
            blob->Append(set.Layout.World);
            blob->Append(set.Layout.RenderLayerMask);
            blob->Append(idle.ContentEpoch);
            blob->Append(idle.LightEpoch);
            blob->Append(idle.DepthDynamicEpoch);
            blob->Append(d.Frame.PhysicalTexture(depth).id);
        }
    }
    const uint32_t reuseView = static_cast<uint32_t>(d.View.id);
    const auto allocation = d.Frame.PhysicalBuffer(requests).id;
    auto& cache = m_RequestCaches.GetOrInsert(reuseView);
    const bool reused = cache.CanReuse(d.Frame.FrameIndex(), domain, inputs, allocation,
                                      needsInitialization, allowReuse);
    auto* marked = d.Frame.AllocatePassData<bool>(reused);
    if (!reused)
    {
        const auto write = cache.PrepareWrite();
        const auto ticket = write.Ticket;
        // An aborted graph must leave the fresh-buffer arm set, so the pool does the clear.
        if (write.Clear)
            d.Frame.AddBufferZeroInit(requests, d.PassName("ClearVirtualShadowRequests").c_str());
        d.Frame.AddPass(
            d.PassName("MarkVirtualShadowRequests").c_str(), PassPhase::kDefault,
            [&, inputs = std::move(inputs)](RenderGraph::RGPassBuilder& builder) mutable
            {
                builder.UsePipeline(m_Pipeline);
                builder.Read(depth, RenderGraph::RGTextureRead::SampledCompute);
                builder.Write(requests, RenderGraph::RGBufferWrite::Storage);
                builder.OnSubmitted([this, reuseView, ticket, allocation, marked, inputs = std::move(inputs)]() mutable
                {
                    if (!*marked)
                        return;
                    if (auto* found = m_RequestCaches.Find(reuseView))
                        found->MarkSubmitted(ticket, std::move(inputs), allocation);
                });
            },
            [=, this, requestBytes = descriptor.size](RenderGraph::RGContext& context)
            {
                auto* recordingDevice = context.GetDevice();
                if (!recordingDevice || !context.Cmd)
                    return;
                const auto pipeline = context.GetOrCreatePipelineVariant(m_Pipeline);
                const auto depthTexture = context.GetTexture(depth);
                const auto requestBuffer = context.GetBuffer(requests);
                if (!pipeline.IsValid() || !depthTexture.IsValid() || !requestBuffer.IsValid())
                    return;
                DescriptorSetDesc setDescriptor{};
                setDescriptor.layout = m_Layout;
                setDescriptor.transient = true;
                setDescriptor.debugName = "VirtualShadowPageRequests.Set0";
                const auto descriptorSet = recordingDevice->CreateDescriptorSet(setDescriptor);
                if (!descriptorSet.IsValid())
                    return;
                NamedDescriptorWriter writer(recordingDevice, descriptorSet, m_Meta, 0);
                writer.AddCombinedImageSampler("DepthTex", depthTexture, m_Sampler)
                      .AddStorageBuffer("ShadowMarkProjections", projectionUpload.Buffer, projectionUpload.Offset,
                                        projectionBytes)
                      .AddStorageBuffer("ShadowPageRequests", requestBuffer, 0, requestBytes)
                      .AddUniformBuffer("ShadowMarkParams", params.Buffer, params.Offset,
                                        sizeof(VirtualShadowMarkParamsGPU));
                writer.Flush();
                context.Cmd->SetPipeline(pipeline);
                context.Cmd->BindDescriptorSet(0, descriptorSet, pipeline);
                context.Cmd->Dispatch((width + kWorkgroupSide - 1) / kWorkgroupSide,
                                      (height + kWorkgroupSide - 1) / kWorkgroupSide, projectionCount);
                *marked = true;
            });
    }

    const auto plan = feature.DeclareFeedback(d.Frame, d.View.id, requests, marked, set.Layout,
        d.Services.GetShadowCasterChanges(set.Layout.World), d.Services.GetMeshGPURegistry().GetContentRevision(),
        d.PassName("ReadVirtualShadowFeedback").c_str());
    const auto table = feature.DeclarePages(d.Frame, d.Services, shadows, plan, set.Layout, set.Cameras);
    if (!table.IsValid() || !plan.Atlas.IsValid())
        return;

    const size_t lookupBytes = sizeof(lookup) + set.Rows.size() * sizeof(VirtualShadowLookupProjectionGPU);
    const auto metadata = d.Frame.AllocUpload(lookupBytes);
    if (!metadata.Valid())
        return;
    auto* bytes = static_cast<std::byte*>(metadata.Ptr);
    std::memcpy(bytes, &lookup, sizeof(lookup));
    for (size_t index = 0; index < set.Rows.size(); ++index)
    {
        const auto row = MakeLookupRow(set.Rows[index], set.Layout.Projections[index], set.Cameras[index], plan.Work);
        std::memcpy(bytes + sizeof(lookup) + index * sizeof(row), &row, sizeof(row));
    }
    VirtualShadowViewInputs published{};
    published.Atlas = plan.Atlas;
    published.PageTable = table;
    published.PageTableBytes = static_cast<uint64_t>(wordCount) * 32u * sizeof(uint32_t);
    published.Projections = metadata.Buffer;
    published.ProjectionsOffset = metadata.Offset;
    published.ProjectionsBytes = lookupBytes;
    published.DepthSampler = m_Sampler;
    published.CompareSampler = compareSampler;
    d.Services.PublishVirtualShadowInputs(shadows.Seam, d.Frame, d.View.id, published);
}
} // namespace GameEngine::Engine::Renderer
