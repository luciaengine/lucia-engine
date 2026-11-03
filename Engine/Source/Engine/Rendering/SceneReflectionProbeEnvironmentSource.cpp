#include "Engine/Rendering/SceneReflectionProbeEnvironmentSource.h"

#include "Engine/Rendering/ImageBasedLightingFeature.h"
#include "Engine/Rendering/PipelineVariantCache.h"
#include "Engine/Rendering/IRenderFeature.h"
#include "Engine/Rendering/RenderOrigin.h"
#include "Engine/Rendering/RenderServices.h"
#include "RenderServicesDetail.h"

#include "Logger/Logger.h"
#include "Mathematics/Matrix4x4.h"
#include "Mathematics/MatrixOps.h"
#include "Mathematics/Vector3.h"
#include "Rendering/CameraDerivation.h"
#include "Rendering/Common/Frustum.h"
#include "Rendering/Common/Utils.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/GPUCulling.h"
#include "Rendering/Core/PassPhase.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Core/SpecializationConstants.h"
#include "Types/Fnv1a.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <chrono>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace GameEngine
{
namespace Engine::Renderer
{
using namespace GameEngine::Rendering;

namespace
{
// Probe-prefixed to stay distinct from SkyEnvironmentSource.cpp's anonymous-namespace
// twins when a unity build merges both files into one translation unit.
constexpr uint32_t kProbeConvolveGroup = 8;
constexpr float kHalfPi = 1.57079632679489661923f;
constexpr float kFallbackDeltaTimeSeconds = 1.0f / 60.0f;
constexpr float kCaptureNearPlane = 0.005f;
constexpr uint32_t kAllFacesMask = (1u << ImageBasedLightingFeature::kNumCaptureFaces) - 1u;
static_assert(ImageBasedLightingFeature::kNumCaptureFaces == kProbeFaceCount,
              "the probe-face cull index block is sized to the capture cube's face count");

struct ProbeCubeFaceBasis
{
    float up[3];
    float forward[3];
};

constexpr ProbeCubeFaceBasis kProbeCaptureFaces[ImageBasedLightingFeature::kNumCaptureFaces] = {
    {{0.0f, 1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}},
    {{0.0f, 1.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}},
    {{0.0f, 0.0f, -1.0f}, {0.0f, 1.0f, 0.0f}},
    {{0.0f, 0.0f, 1.0f}, {0.0f, -1.0f, 0.0f}},
    {{0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}},
    {{0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, -1.0f}},
};


void HashBytes(uint64_t& h, const void* data, size_t bytes)
{
    h = Hashing::Fnv1a64(data, bytes, h);
}

uint64_t MixDigest(uint64_t base, uint32_t generation)
{
    uint64_t h = base != 0 ? base : Hashing::kFnv1a64OffsetBasis;
    HashBytes(h, &generation, sizeof(generation));
    return h != 0 ? h : 1;
}
} // namespace

SceneReflectionProbeEnvironmentSource::SceneReflectionProbeEnvironmentSource(RenderServices& services)
    : m_Services(&services),
      m_SkyBackground(services)
{
}

SceneReflectionProbeEnvironmentSource::~SceneReflectionProbeEnvironmentSource()
{
    if (!m_Services)
        return;

    if (m_ViewId != 0)
    {
        m_Services->Views().ReleaseView(m_ViewId);
        m_ViewId = 0;
    }
    if (m_CameraId != 0)
    {
        m_Services->Views().ReleaseCamera(m_CameraId);
        m_CameraId = 0;
    }
}

void SceneReflectionProbeEnvironmentSource::SetProbe(const SceneReflectionProbeEnvironmentDesc& desc)
{
    const bool worldChanged = desc.WorldId != m_Desc.WorldId;
    m_Desc = desc;
    // Keep the sky background's probe overrides current from frame one:
    // BaseDigest() hashes m_SkyBackground.InputDigest(), whose effective
    // settings fold these in — pushing them only at capture time would shift
    // the digest after the first bake and schedule one spurious rebake.
    SkyEnvironmentProbeOverrides skyOverrides{};
    skyOverrides.Intensity = 1.0f;
    skyOverrides.ExposureEV = CaptureDesc().ExposureEV;
    skyOverrides.RotationRadians = CaptureDesc().RotationRadians;
    skyOverrides.LowerHemisphereDarkness = CaptureDesc().LowerHemisphereDarkness;
    m_SkyBackground.SetProbeOverrides(skyOverrides);
    EnsureViews();

    // A world retarget cancels the old cycle. A new arm must precede capture
    // so extraction and culling are fed by the correct world's entities.
    if (worldChanged && IsBaking())
    {
        m_CaptureState = CaptureState::Idle;
        m_ArmedFaceMask = 0;
        m_LastBaseDigest = 0;
    }
}

bool SceneReflectionProbeEnvironmentSource::EnsureViews()
{
    if (m_ViewsAllocated)
        return true;
    if (!m_Services)
        return false;

    m_CameraId = m_Services->Views().AllocateCamera("Scene Reflection Probe Camera");
    // OnDemand: the view costs nothing until a bake requests it. One view
    // serves all six faces; the camera is re-pointed per face at declaration.
    m_ViewDebugName = "Probe." + std::to_string(m_Desc.SourceKey);
    m_ViewId = m_Services->Views().AllocateView(m_ViewDebugName.c_str(), m_CameraId,
                                                Rendering::ViewPurpose::UtilityCapture,
                                                Rendering::ViewParticipation::OnDemand);
    m_Services->Views().SetViewActiveRenderPipeline(m_ViewId, false);
    m_Services->Views().SetViewRenderLayerMask(m_ViewId, m_Desc.CullMask);
    m_Services->Views().SetViewWorldId(m_ViewId, m_Desc.WorldId);

    m_ViewsAllocated = true;
    return true;
}

void SceneReflectionProbeEnvironmentSource::ArmFaces(uint32_t faceMask)
{
    if (!m_Services || !m_ViewsAllocated || m_ViewId == 0)
        return;
    m_Services->Views().RequestViewFrame(m_ViewId);
    m_Services->Views().SetViewRenderLayerMask(m_ViewId, CaptureDesc().CullMask);
    m_Services->Views().SetViewWorldId(m_ViewId, CaptureDesc().WorldId);
    m_ArmedFaceMask = faceMask & kAllFacesMask;
}


void SceneReflectionProbeEnvironmentSource::ScheduleCulling(const FeatureCullingContext& ctx)
{
    using GameEngine::Mathematics::Vector3;

    // Only the frame(s) the arm requested: the view is fed by extraction then,
    // and the bake that consumes these slices declares later this frame.
    if (!ctx.CullingPipeline || !ctx.Views || m_ViewId == 0 || m_ArmedFaceMask == 0 ||
        m_CaptureState == CaptureState::Idle || ctx.InstanceCount == 0)
        return;
    const Rendering::ViewDesc* view = nullptr;
    for (const Rendering::ViewDesc& v : *ctx.Views)
    {
        if (v.id == m_ViewId)
        {
            view = &v;
            break;
        }
    }
    if (!view || view->ActiveRenderLayerMask() == 0u)
        return;

    // Every face shares the eye and the projection, so one derived camera
    // describes the group; only the frustum planes differ per face. The
    // camera-relative origin is derived from the eye exactly as the spine
    // does for its per-view dispatch (see ScheduleViewCullingDispatches).
    const Rendering::CameraDerivedData shared = Rendering::DeriveCameraData(MakeFaceCamera(0));
    Vector3 origin{};
    {
        const auto originSector =
            ComputeRenderOriginSector(m_Desc.Position[0], m_Desc.Position[1], m_Desc.Position[2]);
        SectorToWorld(originSector, origin.x, origin.y, origin.z);
    }

    // Armed faces pack into runs of consecutive faces, at most
    // kMaxCullingViewsPerDispatch per group: slice c of a run started at face
    // f publishes at ProbeFaceCullingIndex(f + c). All six faces = {0-3, 4-5}.
    uint32_t face = 0;
    while (face < ImageBasedLightingFeature::kNumCaptureFaces)
    {
        if ((m_ArmedFaceMask & (1u << face)) == 0u)
        {
            ++face;
            continue;
        }
        Rendering::CascadeCullingGroup group{};
        group.viewId = m_ViewId;
        group.cascadeIndexBase = ProbeFaceCullingIndex(face);
        group.shadowCasterDispatch = false; // color capture: keep non-casters
        group.cameraPosition = shared.Position;
        group.cameraForward = shared.Forward;
        group.nearPlane = shared.NearPlane;
        group.farPlane = shared.FarPlane;
        group.cameraRelativeOrigin = origin;
        group.firstInstance = 0;
        group.instanceCount = ctx.InstanceCount;
        group.renderLayerMask = view->ActiveRenderLayerMask();
        group.frameIndex = ctx.FrameIndex;
        group.deltaTime = ctx.DeltaTime;
        uint32_t count = 0;
        while (face < ImageBasedLightingFeature::kNumCaptureFaces &&
               count < Rendering::kMaxCullingViewsPerDispatch &&
               (m_ArmedFaceMask & (1u << face)) != 0u)
        {
            const Rendering::CameraDerivedData faceCamera =
                Rendering::DeriveCameraData(MakeFaceCamera(face));
            group.lightVP[count] = faceCamera.ViewProjMatrix;
            Rendering::ExtractFrustumPlanes(faceCamera.ViewProjMatrix, group.frustumPlanes[count]);
            ++count;
            ++face;
        }
        group.cascadeCount = count;
        ctx.CullingPipeline->SubmitCascadeGroup(group);
    }
}

uint64_t SceneReflectionProbeEnvironmentSource::BaseDigest() const
{
    if (!HasActiveContent())
        return 0;

    uint64_t h = Hashing::kFnv1a64OffsetBasis;
    constexpr char kTag[] = "SceneReflectionProbeEnvironmentSource";
    HashBytes(h, kTag, sizeof(kTag) - 1);
    HashBytes(h, &m_Desc.SourceKey, sizeof(m_Desc.SourceKey));
    HashBytes(h, &m_ManualRevision, sizeof(m_ManualRevision));
    HashBytes(h, &m_Desc.SceneEditDigest, sizeof(m_Desc.SceneEditDigest));
    // A Once capture can finish while its materials are still compiling.
    // Revisit it when pass variants actually publish, including cache hits.
    HashBytes(h, &m_MaterialReadyGeneration, sizeof(m_MaterialReadyGeneration));
    const uint64_t materialContent = m_Desc.SceneEditDigest == 0 ? Material::GetGlobalContentEpoch() : 0;
    HashBytes(h, &materialContent, sizeof(materialContent));
    HashBytes(h, &m_Desc.WorldId, sizeof(m_Desc.WorldId));
    HashBytes(h, m_Desc.Position, sizeof(m_Desc.Position));
    HashBytes(h, &m_Desc.InfluenceRadius, sizeof(m_Desc.InfluenceRadius));
    HashBytes(h, &m_Desc.MaxDistance, sizeof(m_Desc.MaxDistance));
    HashBytes(h, &m_Desc.CullMask, sizeof(m_Desc.CullMask));
    HashBytes(h, &m_Desc.CaptureEnvironment, sizeof(m_Desc.CaptureEnvironment));
    HashBytes(h, &m_Desc.ExposureEV, sizeof(m_Desc.ExposureEV));
    HashBytes(h, &m_Desc.RotationRadians, sizeof(m_Desc.RotationRadians));
    HashBytes(h, &m_Desc.CaptureResolution, sizeof(m_Desc.CaptureResolution));
    HashBytes(h, &m_Desc.UpdateMode, sizeof(m_Desc.UpdateMode));
    HashBytes(h, &m_Desc.RealtimeUpdateInterval, sizeof(m_Desc.RealtimeUpdateInterval));
    // The capture's background is the live sky rendered through m_SkyBackground,
    // so the sky's bake inputs are this probe's bake inputs too: a time-of-day or
    // atmosphere edit invalidates the captured cube. Without this, a probe keeps
    // serving the old sky's lighting until some probe field happens to change
    // ("Once" means not continuous, not immune to a changed environment).
    const uint64_t skyDigest = m_SkyBackground.InputDigest();
    HashBytes(h, &skyDigest, sizeof(skyDigest));
    // ALSO hash the sky state the sky-view LUT actually CONTAINS: a bake fired
    // on the change frame can sample the not-yet-recomputed LUT (the recompute
    // may land a frame later or in another window's stream) and silently keep
    // the previous sky. When the LUT catches up this digest moves again and
    // the corrective rebake fires — convergence instead of a stale capture.
    const uint64_t lutDigest = m_SkyBackground.MaterializedLutDigest();
    HashBytes(h, &lutDigest, sizeof(lutDigest));
    // ECS-owned probes supply a spatially scoped scene digest. Direct users
    // without that digest retain the conservative world-membership fallback.
    const uint64_t contentDigest = m_Services && m_Desc.SceneEditDigest == 0
        ? m_Services->GetWorldRenderContentDigest(m_Desc.WorldId)
        : 0;
    HashBytes(h, &contentDigest, sizeof(contentDigest));
    return h != 0 ? h : 1;
}

uint64_t SceneReflectionProbeEnvironmentSource::InputDigest() const
{
    return MixDigest(BaseDigest(), m_CaptureGeneration);
}

SceneProbeWorldEpochs SceneReflectionProbeEnvironmentSource::WorldEpochs() const
{
    // World-scoped, not range-scoped: a change anywhere in the probe's world
    // counts (SceneProbeWorldEpochs lists what moves them and what does not).
    return m_Services ? SceneProbeWorldEpochs::Read(*m_Services, m_Desc.WorldId) : SceneProbeWorldEpochs{};
}

void SceneReflectionProbeEnvironmentSource::TickBakeClock(float deltaTimeSeconds)
{
    m_SecondsSinceSceneBake += deltaTimeSeconds > 0.0f ? deltaTimeSeconds : kFallbackDeltaTimeSeconds;
}

float SceneReflectionProbeEnvironmentSource::RealtimeInterval() const
{
    return m_Desc.UpdateMode == SceneReflectionProbeUpdateMode::Realtime
               ? std::max(0.0f, m_Desc.RealtimeUpdateInterval)
               : -1.0f;
}

bool SceneReflectionProbeEnvironmentSource::BakeDue() const
{
    const uint64_t base = BaseDigest();
    if (base == 0)
        return false;
    if (base != m_LastBaseDigest)
        return true;
    if (m_Desc.UpdateMode == SceneReflectionProbeUpdateMode::Realtime)
    {
        const float interval = std::max(0.0f, m_Desc.RealtimeUpdateInterval);
        return interval <= 0.0f || m_SecondsSinceSceneBake >= interval;
    }
    return false;
}

uint64_t SceneReflectionProbeEnvironmentSource::NextBakeDigest()
{
    const uint64_t base = BaseDigest();
    if (base == 0)
        return 0;

    if (BakeDue())
    {
        m_LastBaseDigest = base;
        ++m_CaptureGeneration;
        if (m_CaptureGeneration == 0)
            m_CaptureGeneration = 1;
    }

    return MixDigest(base, m_CaptureGeneration);
}

EnvironmentLocalReflectionData SceneReflectionProbeEnvironmentSource::LocalReflectionData() const
{
    EnvironmentLocalReflectionData data{};
    const bool validBox = m_Desc.BoxProjection &&
                          m_Desc.BoxHalfExtents[0] > 0.0001f &&
                          m_Desc.BoxHalfExtents[1] > 0.0001f &&
                          m_Desc.BoxHalfExtents[2] > 0.0001f;
    if (!validBox)
        return data;

    data.BoxProjection = true;
    std::memcpy(data.ProbePositionWS, m_Desc.Position, sizeof(data.ProbePositionWS));
    std::memcpy(data.BoxCenterWS, m_Desc.BoxCenter, sizeof(data.BoxCenterWS));
    std::memcpy(data.BoxHalfExtentsWS, m_Desc.BoxHalfExtents, sizeof(data.BoxHalfExtentsWS));
    std::memcpy(data.BoxAxisXWS, m_Desc.BoxAxisX, sizeof(data.BoxAxisXWS));
    std::memcpy(data.BoxAxisYWS, m_Desc.BoxAxisY, sizeof(data.BoxAxisYWS));
    std::memcpy(data.BoxAxisZWS, m_Desc.BoxAxisZ, sizeof(data.BoxAxisZWS));
    return data;
}

bool SceneReflectionProbeEnvironmentSource::EnsurePipelines(IDevice& device,
                                                            const ImageBasedLightingFeature& feature)
{
    const uint32_t captureResolution = feature.GetEnvCaptureSize();
    const uint32_t prefilterMipCount = feature.GetPrefilterMipCount();
    if (m_PipelinesReady &&
        m_PipelineCaptureResolution == captureResolution &&
        m_PipelinePrefilterMipCount == prefilterMipCount)
        return true;

    std::vector<uint8_t> csDownsample = Utils::LoadShaderFile("Shaders/sky_env_mip_downsample.comp.spv");
    std::vector<uint8_t> csDiffuse = Utils::LoadShaderFile("Shaders/sky_diffuse_convolve.comp.spv");
    std::vector<uint8_t> csSpecular = Utils::LoadShaderFile("Shaders/sky_specular_prefilter.comp.spv");
    if (csDownsample.empty() || csDiffuse.empty() || csSpecular.empty())
    {
        static bool warned = false;
        if (!warned)
        {
            warned = true;
            Logger::Log::Warning("SceneReflectionProbeEnvironmentSource: missing bake shader SPIR-V; IBL will keep the current fallback");
        }
        return false;
    }

    const DescriptorSetLayoutId convolveLayout = device.InternDescriptorSetLayout(ImageBasedLightingFeature::ConvolveSet0());

    ComputePipelineDesc md{};
    md.ComputeShader = std::make_shared<const std::vector<uint8_t>>(std::move(csDownsample));
    md.DescriptorSetLayouts.push_back(convolveLayout);
    md.DebugName = "IBL_SceneProbeEnvMipDownsample";
    m_MipDownsamplePipelineId = device.InternComputePipeline(md);

    ComputePipelineDesc dd{};
    dd.ComputeShader = std::make_shared<const std::vector<uint8_t>>(std::move(csDiffuse));
    dd.DescriptorSetLayouts.push_back(convolveLayout);
    dd.DebugName = "IBL_SceneProbeDiffuseConvolve";
    SpecializationConstants diffuseSpecConstants;
    diffuseSpecConstants.AddConstant<uint32_t>(0u, captureResolution, "kEnvCaptureSize");
    dd.Specialization = std::move(diffuseSpecConstants);
    m_DiffusePipelineId = device.InternComputePipeline(dd);

    ComputePipelineDesc sd{};
    sd.ComputeShader = std::make_shared<const std::vector<uint8_t>>(std::move(csSpecular));
    sd.DescriptorSetLayouts.push_back(convolveLayout);
    sd.DebugName = "IBL_SceneProbeSpecularPrefilter";
    SpecializationConstants specConstants;
    specConstants.AddConstant<uint32_t>(0u, feature.GetPrefilterSize(), "kSpecBaseSize");
    specConstants.AddConstant<uint32_t>(1u, prefilterMipCount, "kSpecMipCount");
    sd.Specialization = std::move(specConstants);
    m_SpecularPipelineId = device.InternComputePipeline(sd);

    m_PipelinesReady = m_MipDownsamplePipelineId.IsValid() && m_DiffusePipelineId.IsValid() &&
                       m_SpecularPipelineId.IsValid();
    if (m_PipelinesReady)
    {
        m_PipelineCaptureResolution = captureResolution;
        m_PipelinePrefilterMipCount = prefilterMipCount;
    }
    return m_PipelinesReady;
}

CameraData SceneReflectionProbeEnvironmentSource::MakeFaceCamera(uint32_t face) const
{
    using GameEngine::Mathematics::MakeLookAtLH;
    using GameEngine::Mathematics::MakePerspectiveLH_ZO_ReverseZ;
    using GameEngine::Mathematics::Matrix4x4;
    using GameEngine::Mathematics::Vector3;

    const ProbeCubeFaceBasis& basis = kProbeCaptureFaces[face % ImageBasedLightingFeature::kNumCaptureFaces];
    const Vector3 eye(CaptureDesc().Position[0], CaptureDesc().Position[1], CaptureDesc().Position[2]);
    const Vector3 forward(basis.forward[0], basis.forward[1], basis.forward[2]);
    const Vector3 up(basis.up[0], basis.up[1], basis.up[2]);
    const Vector3 target(eye.x + forward.x, eye.y + forward.y, eye.z + forward.z);
    const Matrix4x4 view = MakeLookAtLH(eye, target, up);
    float fallbackFar = CaptureDesc().InfluenceRadius;
    if (CaptureDesc().BoxProjection)
    {
        const float rel[3] = {
            CaptureDesc().Position[0] - CaptureDesc().BoxCenter[0],
            CaptureDesc().Position[1] - CaptureDesc().BoxCenter[1],
            CaptureDesc().Position[2] - CaptureDesc().BoxCenter[2]};
        const float local[3] = {
            rel[0] * CaptureDesc().BoxAxisX[0] + rel[1] * CaptureDesc().BoxAxisX[1] + rel[2] * CaptureDesc().BoxAxisX[2],
            rel[0] * CaptureDesc().BoxAxisY[0] + rel[1] * CaptureDesc().BoxAxisY[1] + rel[2] * CaptureDesc().BoxAxisY[2],
            rel[0] * CaptureDesc().BoxAxisZ[0] + rel[1] * CaptureDesc().BoxAxisZ[1] + rel[2] * CaptureDesc().BoxAxisZ[2]};
        const float dx = CaptureDesc().BoxHalfExtents[0] + std::fabs(local[0]);
        const float dy = CaptureDesc().BoxHalfExtents[1] + std::fabs(local[1]);
        const float dz = CaptureDesc().BoxHalfExtents[2] + std::fabs(local[2]);
        fallbackFar = std::max(fallbackFar, std::sqrt(dx * dx + dy * dy + dz * dz));
    }
    const float requestedFar = CaptureDesc().MaxDistance > 0.0f ? CaptureDesc().MaxDistance : fallbackFar;
    const float farZ = std::max(requestedFar, 1.0f);
    const Matrix4x4 proj = MakePerspectiveLH_ZO_ReverseZ(kHalfPi, 1.0f, kCaptureNearPlane, farZ);
    const Matrix4x4 viewProj = proj * view;

    CameraData out{};
    std::memcpy(out.view, view.Data(), sizeof(float) * 16u);
    std::memcpy(out.proj, proj.Data(), sizeof(float) * 16u);
    std::memcpy(out.viewProj, viewProj.Data(), sizeof(float) * 16u);
    out.cameraPos[0] = eye.x;
    out.cameraPos[1] = eye.y;
    out.cameraPos[2] = eye.z;
    out.cameraPos[3] = 0.0f;
    return out;
}

bool SceneReflectionProbeEnvironmentSource::RecordMipDownsample(RenderGraph::RGContext& ctx,
                                                                ImageBasedLightingFeature& feature,
                                                                uint32_t dstMip) const
{
    auto* cl = ctx.Cmd;
    auto* dev = ctx.GetDevice();
    if (!cl || !dev || dstMip == 0)
        return false;

    TextureViewHandle store = feature.GetEnvCaptureStoreView(dstMip);
    // Single-mip source view: the sampled descriptor's claim must cover ONLY
    // the RG-declared Read(mip dstMip-1) — the whole-chain view would claim
    // ShaderReadOnly for the GENERAL-resident destination mip too (VUID-00344).
    TextureViewHandle source = feature.GetEnvCaptureMipSampleView(dstMip - 1);
    if (!store.IsValid() || !source.IsValid())
        return false;

    PipelineHandle pipe = ctx.GetOrCreatePipelineVariant(m_MipDownsamplePipelineId);
    if (!pipe.IsValid())
        return false;
    cl->SetPipeline(pipe);

    DescriptorSetDesc dsd{};
    dsd.layout = ImageBasedLightingFeature::ConvolveSet0();
    dsd.transient = true;
    dsd.debugName = "IBL_SceneProbeEnvMipDownsample_DS0";
    DescriptorSetHandle ds = dev->CreateDescriptorSet(dsd);
    dev->UpdateCombinedImageSamplerBinding(ds, 0, source, feature.GetCubeSampler());
    dev->UpdateStorageImageBinding(ds, 1, store);
    cl->BindDescriptorSet(0, ds, pipe);

    const uint32_t size = feature.GetEnvCaptureSize() >> dstMip;
    const uint32_t g = ((size > 0 ? size : 1u) + kProbeConvolveGroup - 1) / kProbeConvolveGroup;
    cl->Dispatch(g, g, ImageBasedLightingFeature::kNumCaptureFaces);
    return true;
}

bool SceneReflectionProbeEnvironmentSource::RecordDiffuseConvolve(RenderGraph::RGContext& ctx,
                                                                  ImageBasedLightingFeature& feature) const
{
    auto* cl = ctx.Cmd;
    auto* dev = ctx.GetDevice();
    if (!cl || !dev)
        return false;

    PipelineHandle pipe = ctx.GetOrCreatePipelineVariant(m_DiffusePipelineId);
    if (!pipe.IsValid())
        return false;
    cl->SetPipeline(pipe);

    DescriptorSetDesc dsd{};
    dsd.layout = ImageBasedLightingFeature::ConvolveSet0();
    dsd.transient = true;
    dsd.debugName = "IBL_SceneProbeDiffuseConvolve_DS0";
    DescriptorSetHandle ds = dev->CreateDescriptorSet(dsd);
    dev->UpdateCombinedImageSamplerBinding(ds, 0, feature.GetEnvCaptureCubeView(), feature.GetCubeSampler());
    dev->UpdateStorageImageBinding(ds, 1, feature.GetIrradianceStoreView());
    cl->BindDescriptorSet(0, ds, pipe);

    const uint32_t g = (ImageBasedLightingFeature::kIrradianceSize + kProbeConvolveGroup - 1) / kProbeConvolveGroup;
    cl->Dispatch(g, g, ImageBasedLightingFeature::kNumCaptureFaces);
    return true;
}

bool SceneReflectionProbeEnvironmentSource::RecordSpecularPrefilter(RenderGraph::RGContext& ctx,
                                                                    ImageBasedLightingFeature& feature,
                                                                    uint32_t mip) const
{
    auto* cl = ctx.Cmd;
    auto* dev = ctx.GetDevice();
    if (!cl || !dev)
        return false;

    PipelineHandle pipe = ctx.GetOrCreatePipelineVariant(m_SpecularPipelineId);
    if (!pipe.IsValid())
        return false;
    cl->SetPipeline(pipe);

    {
        TextureViewHandle store = feature.GetPrefilterStoreView(mip);
        if (!store.IsValid())
            return false;

        DescriptorSetDesc dsd{};
        dsd.layout = ImageBasedLightingFeature::ConvolveSet0();
        dsd.transient = true;
        dsd.debugName = "IBL_SceneProbeSpecularPrefilter_DS0";
        DescriptorSetHandle ds = dev->CreateDescriptorSet(dsd);
        dev->UpdateCombinedImageSamplerBinding(ds, 0, feature.GetEnvCaptureCubeView(), feature.GetCubeSampler());
        dev->UpdateStorageImageBinding(ds, 1, store);
        cl->BindDescriptorSet(0, ds, pipe);

        const uint32_t size = feature.GetPrefilterSize() >> mip;
        const uint32_t g = (size + kProbeConvolveGroup - 1) / kProbeConvolveGroup;
        cl->Dispatch(g, g, ImageBasedLightingFeature::kNumCaptureFaces);
    }


    return true;
}

bool SceneReflectionProbeEnvironmentSource::DeclareCaptureFrame(const EnvironmentBakeContext& ctx,
                                                                uint32_t faceMask)
{
    // The slices select LODs from the view's camera: every face shares the
    // eye and the projection, so any armed face's camera describes them all.
    uint8_t sliceIndices[ImageBasedLightingFeature::kNumCaptureFaces];
    uint32_t sliceCount = 0;
    for (uint32_t face = 0; face < ImageBasedLightingFeature::kNumCaptureFaces; ++face)
    {
        if ((faceMask & (1u << face)) != 0u)
            sliceIndices[sliceCount++] = ProbeFaceCullingIndex(face);
    }
    if (sliceCount == 0)
        return false;

    const uint32_t firstFace = static_cast<uint32_t>(sliceIndices[0] - kProbeFaceCullingIndexBase);
    m_Services->Views().SetCameraData(m_CameraId, MakeFaceCamera(firstFace));
    m_Services->Views().SetViewRenderLayerMask(m_ViewId, CaptureDesc().CullMask);
    m_Services->Views().SetViewWorldId(m_ViewId, CaptureDesc().WorldId);

    m_Services->WriteViewLightBuffer(m_ViewId);
    m_Services->BuildWorldBatchKeysForView(m_ViewId);
    return m_Services->ScheduleBucketerDispatchesForViewSlices(
        *ctx.Frame, m_ViewId, std::span<const uint8_t>(sliceIndices, sliceCount));
}

bool SceneReflectionProbeEnvironmentSource::DeclareFaceCapture(
    const EnvironmentBakeContext& ctx, RenderGraph::RGTexture envCube, uint32_t face)
{
    RenderGraph::RGFrame& frame = *ctx.Frame;
    ImageBasedLightingFeature& feature = *ctx.Feature;

    const std::string skyName = "ProbeSky." + std::to_string(CaptureDesc().SourceKey);
    const bool hasSkyFace = m_SkyBackground.ScheduleCaptureFacesOnly(
        ctx, skyName.c_str(), PassPhase::kEarlySetup, face);

    // Per-face registry state, consumed at declaration by this face's forward
    // emit and world pass (camera upload, clear ops), so re-pointing the one
    // view per face is exact.
    m_Services->Views().SetCameraData(m_CameraId, MakeFaceCamera(face));

    Rendering::ViewClearConfig clear{};
    clear.clearColor = !hasSkyFace;
    clear.clearColorValue[0] = 0.0f;
    clear.clearColorValue[1] = 0.0f;
    clear.clearColorValue[2] = 0.0f;
    clear.clearColorValue[3] = 1.0f;
    clear.clearDepth = true;
    clear.clearDepthValue = 0.0f;
    m_Services->Views().SetViewClearConfig(m_ViewId, clear);

    TextureDesc depthDesc{};
    depthDesc.width = feature.GetEnvCaptureSize();
    depthDesc.height = feature.GetEnvCaptureSize();
    depthDesc.depth = 1;
    depthDesc.mipLevels = 1;
    depthDesc.arrayLayers = 1;
    depthDesc.sampleCount = 1;
    depthDesc.format = static_cast<uint32_t>(m_Services->GetDepthFormat());
    depthDesc.usage = static_cast<uint32_t>(TextureUsage::DepthStencil | TextureUsage::ShaderResource);
    depthDesc.debugName = "IBL.SceneProbe.Depth";
    const std::string depthName = "IBL.SceneProbe." + std::to_string(CaptureDesc().SourceKey) + ".Depth.Face" + std::to_string(face);
    const RenderGraph::RGTexture depth = frame.ImportPersistentTexture(depthName.c_str(), depthDesc);
    if (!depth.IsValid())
        return false;

    RenderServices::WorldPassTargetsRG targets{};
    targets.Color = envCube;
    targets.Depth = depth;
    targets.ColorRange.BaseMip = 0;
    targets.ColorRange.MipCount = 1;
    targets.ColorRange.BaseLayer = face;
    targets.ColorRange.LayerCount = 1;

    // Forward commands are per view and replaced on every emit, so the pass
    // must directly follow its own face's emit.
    m_Services->EmitProducerForwardCommandsForView(
        m_ViewId, ForwardEmitPurpose::ReflectionProbeCapture);
    // Break the capture->cube->capture feedback: unless the probe opts in, the
    // capture face binds a zero-intensity EnvData so it never samples the cube
    // it is baking. Scoped to exactly this world pass declaration.
    m_Services->SetWorldPassExcludeEnvironment(!CaptureDesc().CaptureEnvironment);
    const auto world = m_Services->AddWorldPassForView(
        frame, m_ViewId, targets, MaterialKeyword::Instanced,
        RenderServices::WorldPassDrawScope::All, ProbeFaceCullingIndex(face));
    m_Services->SetWorldPassExcludeEnvironment(false);
    return world.Pass.IsValid();
}

void SceneReflectionProbeEnvironmentSource::TickPending(float deltaTimeSeconds)
{
    TickBakeClock(deltaTimeSeconds);
    const float dt = std::max(0.0f, deltaTimeSeconds);
    m_AgeSeconds += dt;
    if (NeedsBake()) m_WaitSeconds += dt;
    else m_WaitSeconds = 0;
}

SceneReflectionProbeEnvironmentSource::Diagnostics SceneReflectionProbeEnvironmentSource::GetDiagnostics() const
{
    return {m_CaptureState == CaptureState::Idle ? "Idle" :
            m_CaptureState == CaptureState::Slicing ? "Capturing face" : "Filtering",
            m_CaptureState == CaptureState::Slicing ? m_SliceFace : m_FilterStep,
            m_CompletedBakes, m_AgeSeconds, m_WaitSeconds, m_CaptureRecordMs, m_FilterRecordMs,
            m_LastCaptureGpuSpanMs, m_LastFilterGpuSpanMs, m_GpuSpansShared};
}

void SceneReflectionProbeEnvironmentSource::ObserveGpuTiming(const char* passName, double gpuSpanMs, bool shared)
{
    const std::string name(passName);
    const std::string key = std::to_string(m_Desc.SourceKey);
    if (name.starts_with("RenderEntities[Probe." + key + "#"))
        m_LastCaptureGpuSpanMs = gpuSpanMs;
    else if (name.starts_with("ReflectionProbe." + key + "."))
        m_LastFilterGpuSpanMs = gpuSpanMs;
    else return;
    m_GpuSpansShared = shared;
}

double SceneReflectionProbeEnvironmentSource::SchedulingScore() const
{
    bool visible = false;
    if (m_Services)
        for (const auto& view : m_Services->Views().GetViews())
        {
            if (!view.activeRenderPipeline || view.worldId != m_Desc.WorldId) continue;
            const auto* camera = m_Services->Views().FindCameraData(view.cameraId);
            if (!camera) continue;
            Rendering::Vector4 planes[6];
            Rendering::ExtractFrustumPlanes(Rendering::DeriveCameraData(*camera).ViewProjMatrix, planes);
            const Rendering::Vector3 position(m_Desc.BoxCenter[0], m_Desc.BoxCenter[1], m_Desc.BoxCenter[2]);
            if (Rendering::TestSphereFrustum(position, m_Desc.InfluenceRadius, planes)) { visible = true; break; }
        }
    const bool changed = BaseDigest() != m_LastBaseDigest;
    return m_WaitSeconds + (visible ? 0.5 : 0.0) + (changed ? 0.5 : 0.0) +
           std::min(m_Desc.Priority, 100u) * 0.005 + (m_CompletedBakes == 0 ? 1.0 : 0.0);
}

void SceneReflectionProbeEnvironmentSource::ResetDevice(IDevice* device)
{
    if (m_Staging) m_Staging->OnDeviceRebuilt(device);
    m_CaptureState = CaptureState::Idle;
    m_ArmedFaceMask = 0;
    m_LastBaseDigest = 0;
    m_PipelinesReady = false;
}

void SceneReflectionProbeEnvironmentSource::ScheduleBake(const EnvironmentBakeContext& input)
{
    if (!input.Frame || !input.Feature || !input.Device || !m_Services || !HasActiveContent()) return;
    if (!m_VariantSubscribed)
    {
        m_VariantPublishedSub = m_Services->Materials().Variants().VariantPublished.Subscribe(
            [this](const GUID& key) {
                if (m_Desc.SceneEditDigest == 0 || std::find(m_Desc.MaterialKeys.begin(), m_Desc.MaterialKeys.end(), key) != m_Desc.MaterialKeys.end())
                    ++m_MaterialReadyGeneration;
            });
        m_VariantSubscribed = true;
    }
    if (!EnsureViews()) return;
    if (!m_Staging) m_Staging = std::make_unique<ImageBasedLightingFeature>();
    if (!m_Staging->Initialize(input.Device)) return;
    if (m_CaptureState == CaptureState::Idle)
    {
        if (!BakeDue()) return;
        if (!m_Staging->EnsureCaptureResolution(m_Desc.CaptureResolution)) return;
        // Hold capture position, range, masks and background overrides for the
        // full six-face cycle. Edits arriving mid-cycle remain queued for the
        // next bake rather than mixing different camera origins in one cube.
        m_CaptureDesc = m_Desc;
        m_SliceDigest = NextBakeDigest();
        m_SliceFace = 0;
        m_FilterStep = 0;
        m_CaptureRecordMs = m_FilterRecordMs = 0;
        m_CaptureState = CaptureState::Slicing;
        ArmFaces(1u);
        return;
    }
    auto ctx = input;
    ctx.Feature = m_Staging.get();
    auto& feature = *ctx.Feature;
    auto& frame = *ctx.Frame;
    if (!EnsurePipelines(*ctx.Device, feature))
    {
        if (m_CaptureState == CaptureState::Slicing) ArmFaces(1u << m_SliceFace);
        return;
    }
    const uint32_t mipSteps = feature.GetEnvCaptureMipCount() - 1;
    const uint32_t totalFilterSteps = mipSteps + 1 + feature.GetPrefilterMipCount();
    if (m_CaptureState == CaptureState::Filtering && m_FilterStep == totalFilterSteps)
    {
        input.Feature->PublishCaptureFrom(feature);
        input.Feature->SetLastBakedDigest(m_SliceDigest);
        input.Feature->ResetFramesSinceBake();
        std::copy_n(m_CaptureDesc.Position, 3, m_PublishedPosition);
        ++m_CompletedBakes;
        m_AgeSeconds = m_WaitSeconds = 0;
        m_CaptureState = CaptureState::Idle;
        return;
    }
    const auto envCube = frame.ImportExternalTexture("IBL_ProbeStaging", feature.GetEnvCaptureTex(),
        ResourceState::ShaderResource, TextureFormat::R16G16B16A16_FLOAT,
        feature.GetEnvCaptureMipCount(), ImageBasedLightingFeature::kNumCaptureFaces);
    if (m_CaptureState == CaptureState::Slicing)
    {
        const auto begin = std::chrono::steady_clock::now();
        if (!DeclareCaptureFrame(ctx, 1u << m_SliceFace) || !DeclareFaceCapture(ctx, envCube, m_SliceFace))
        {
            frame.MarkOutput(envCube, RenderGraph::RGImageLayout::ShaderReadOnly);
            ArmFaces(1u << m_SliceFace);
            return;
        }
        m_CaptureRecordMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
        frame.MarkOutput(envCube, RenderGraph::RGImageLayout::ShaderReadOnly);
        if (++m_SliceFace < ImageBasedLightingFeature::kNumCaptureFaces) ArmFaces(1u << m_SliceFace);
        else { m_ArmedFaceMask = 0; m_CaptureState = CaptureState::Filtering; }
        return;
    }
    const uint32_t step = m_FilterStep;
    const bool downsample = step < mipSteps;
    const bool diffuse = step == mipSteps;
    const uint32_t mip = downsample ? step + 1 : (diffuse ? 0 : step - mipSteps - 1);
    RenderGraph::RGTexture output = envCube;
    if (!downsample)
        output = frame.ImportExternalTexture(diffuse ? "IBL_ProbeStagingDiffuse" : "IBL_ProbeStagingSpecular",
            diffuse ? feature.GetIrradianceCube() : feature.GetPrefilterCube(), ResourceState::ShaderResource,
            TextureFormat::R16G16B16A16_FLOAT, diffuse ? 1 : feature.GetPrefilterMipCount(),
            ImageBasedLightingFeature::kNumCaptureFaces);
    const std::string name = "ReflectionProbe." + std::to_string(m_Desc.SourceKey) +
        (downsample ? ".Downsample." : diffuse ? ".Diffuse." : ".Specular.") + std::to_string(mip);
    frame.AddComputePass(name.c_str(), PassPhase::kWorldRender + 1,
        [=](RenderGraph::RGPassBuilder& p) {
            auto sourceRange = RenderGraph::RGRange::All();
            if (downsample) { sourceRange.BaseMip = mip - 1; sourceRange.MipCount = 1; }
            auto targetRange = RenderGraph::RGRange::All();
            targetRange.BaseMip = mip; targetRange.MipCount = 1;
            p.Read(envCube, RenderGraph::RGTextureRead::Sampled, sourceRange);
            p.Write(output, RenderGraph::RGTextureWrite::Storage, targetRange);
            p.PreventCulling();
        },
        [this, feature = &feature, downsample, diffuse, mip](RenderGraph::RGContext& c) {
            const auto begin = std::chrono::steady_clock::now();
            const bool recorded = downsample ? RecordMipDownsample(c, *feature, mip) :
                diffuse ? RecordDiffuseConvolve(c, *feature) : RecordSpecularPrefilter(c, *feature, mip);
            m_FilterRecordMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
            if (recorded) ++m_FilterStep;
        });
    frame.MarkOutput(envCube, RenderGraph::RGImageLayout::ShaderReadOnly);
    if (!downsample) frame.MarkOutput(output, RenderGraph::RGImageLayout::ShaderReadOnly);
}

} // namespace Engine::Renderer
} // namespace GameEngine
