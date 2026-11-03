#pragma once

#include "Engine/Rendering/IEnvironmentSource.h"
#include "Engine/Rendering/SceneProbeRecaptureSchedule.h"
#include "Events/Event.h"
#include "Engine/Rendering/SkyEnvironmentSource.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineIdentifiers.h"
#include "Rendering/CameraTypes.h"

#include <cstdint>
#include <memory>
#include <vector>
#include <string>
#include "AssetCore/GUID.h"

namespace GameEngine
{
namespace Rendering::RenderGraph
{
class RGContext;
}

namespace Engine::Renderer
{

class ImageBasedLightingFeature;
class RenderServices;
struct FeatureCullingContext;

enum class SceneReflectionProbeUpdateMode : uint32_t
{
    Once = 0,
    Realtime = 1,
};

struct SceneReflectionProbeEnvironmentDesc
{
    float Position[3] = {0.0f, 0.0f, 0.0f};
    float InfluenceRadius = 12.0f;
    bool BoxProjection = true;
    float BoxCenter[3] = {0.0f, 0.0f, 0.0f};
    float BoxHalfExtents[3] = {0.0f, 0.0f, 0.0f};
    float BoxAxisX[3] = {1.0f, 0.0f, 0.0f};
    float BoxAxisY[3] = {0.0f, 1.0f, 0.0f};
    float BoxAxisZ[3] = {0.0f, 0.0f, 1.0f};
    uint32_t InfluenceShape = 1; // 1 box, 2 ellipsoid
    float ProjectionCenter[3] = {0.0f, 0.0f, 0.0f};
    float ProjectionHalfExtents[3] = {0.0f, 0.0f, 0.0f}; // zero inherits influence bounds
    uint64_t SceneEditDigest = 0;
    std::vector<GUID> MaterialKeys;
    float BlendDistance = 2.0f;
    uint32_t Priority = 0;
    float MaxDistance = 0.0f;
    uint32_t CullMask = 0xFFFFFFFFu;
    uint64_t WorldId = 0;
    uint64_t SourceKey = 0;
    float Intensity = 1.0f;
    float ExposureEV = 0.0f;
    float RotationRadians = 0.0f;
    float LowerHemisphereDarkness = 1.0f;
    uint32_t CaptureResolution = 256;
    // See Components::ReflectionProbe::CaptureEnvironment. False excludes the
    // environment cube from the capture's shading (recursion guard).
    bool CaptureEnvironment = false;
    SceneReflectionProbeUpdateMode UpdateMode = SceneReflectionProbeUpdateMode::Realtime;
    float RealtimeUpdateInterval = 0.5f;
};

// Captures the current scene from the probe position into its own IBL
// capture cube, using the active sky/HDRI as the background and then rendering
// world geometry over the six cubemap faces.
class SceneReflectionProbeEnvironmentSource final : public IEnvironmentSource
{
  public:
    explicit SceneReflectionProbeEnvironmentSource(RenderServices& services);
    ~SceneReflectionProbeEnvironmentSource() override;

    const char* Name() const override { return "SceneReflectionProbeEnvironmentSource"; }
    uint64_t InputDigest() const override;
    bool HasActiveContent() const override { return m_Desc.SourceKey != 0; }
    float IblIntensity() const override { return m_Desc.Intensity; }
    float LowerHemisphereDarkness() const override { return m_Desc.LowerHemisphereDarkness; }
    EnvironmentLocalReflectionData LocalReflectionData() const override;
    void ScheduleBake(const EnvironmentBakeContext& ctx) override;
    // One fused CascadeCullingGroup per run of armed faces (faces 0-3 and 4-5
    // for a full bake, one face while slicing), each face a color slice at
    // ProbeFaceCullingIndex(face) of the single capture view.
    void ScheduleCulling(const FeatureCullingContext& ctx) override;

    void TickPending(float deltaTimeSeconds);
    bool NeedsBake() const { return m_CaptureState != CaptureState::Idle || BakeDue(); }
    bool IsBaking() const { return m_CaptureState != CaptureState::Idle; }
    void RequestRebake() { ++m_ManualRevision; }
    void ResetDevice(GameEngine::Rendering::IDevice* device);
    struct Diagnostics
    {
        const char* Stage = "Idle";
        uint32_t Step = 0;
        uint64_t CompletedBakes = 0;
        float AgeSeconds = 0;
        float WaitSeconds = 0;
        double CaptureRecordMs = 0;
        double FilterRecordMs = 0;
        double LastCaptureGpuSpanMs = -1;
        double LastFilterGpuSpanMs = -1;
        bool GpuSpansShared = false;
    };
    Diagnostics GetDiagnostics() const;
    void ObserveGpuTiming(const char* passName, double gpuSpanMs, bool shared);
    // Aging dominates finite visibility/importance bonuses, preventing starvation.
    double SchedulingScore() const;

    bool IsCaptureView(uint32_t viewId) const { return m_ViewsAllocated && m_ViewId == viewId; }

    void SetProbe(const SceneReflectionProbeEnvironmentDesc& desc);
    const SceneReflectionProbeEnvironmentDesc& GetProbe() const { return m_Desc; }
    // Parallax correction must use the origin that produced the visible cube,
    // even if the artist moved the probe while its replacement is still baking.
    const float* GetCapturedPosition() const
    { return m_CompletedBakes > 0 ? m_PublishedPosition : m_Desc.Position; }


  private:
    // One globally granted work unit per frame. Six individually armed faces,
    // then one downsample/convolution/prefilter dispatch per unit. The visible
    // set is swapped only after all filtering has recorded successfully.
    enum class CaptureState : uint8_t { Idle, Slicing, Filtering };

    bool EnsureViews();
    // Request the view frame + world/mask stamp, and record which faces the
    // next frame's ScheduleCulling submits. No state change.
    void ArmFaces(uint32_t faceMask);
    // Per-frame shared prelude for the armed faces: camera/mask/world stamp,
    // light buffer, batch keys, and the ONE bucketer call registering a
    // Color-table slice per face. Must precede every DeclareFaceCapture.
    bool DeclareCaptureFrame(const EnvironmentBakeContext& ctx, uint32_t faceMask);
    // One face's world capture: sky background face, face camera + clear,
    // this face's forward emit, and the world pass drawing the face's slice
    // into envCube layer `face`.
    bool DeclareFaceCapture(const EnvironmentBakeContext& ctx,
                            GameEngine::Rendering::RenderGraph::RGTexture envCube,
                            uint32_t face);
    bool BakeDue() const;
    void TickBakeClock(float deltaTimeSeconds);
    bool EnsurePipelines(GameEngine::Rendering::IDevice& device,
                         const ImageBasedLightingFeature& feature);
    GameEngine::Rendering::CameraData MakeFaceCamera(uint32_t face) const;
    uint64_t BaseDigest() const;
    SceneProbeWorldEpochs WorldEpochs() const;
    // The schedule's interval argument: < 0 for a Once probe.
    float RealtimeInterval() const;
    uint64_t NextBakeDigest();
    bool RecordMipDownsample(GameEngine::Rendering::RenderGraph::RGContext& ctx,
                             ImageBasedLightingFeature& feature, uint32_t dstMip) const;
    bool RecordDiffuseConvolve(GameEngine::Rendering::RenderGraph::RGContext& ctx,
                               ImageBasedLightingFeature& feature) const;
    bool RecordSpecularPrefilter(GameEngine::Rendering::RenderGraph::RGContext& ctx,
                                 ImageBasedLightingFeature& feature, uint32_t mip) const;

    std::unique_ptr<ImageBasedLightingFeature> m_Staging;
    uint64_t m_ManualRevision = 0;
    uint32_t m_FilterStep = 0;
    uint64_t m_CompletedBakes = 0;
    float m_AgeSeconds = 0;
    float m_WaitSeconds = 0;
    double m_CaptureRecordMs = 0;
    double m_FilterRecordMs = 0;
    double m_LastCaptureGpuSpanMs = -1;
    double m_LastFilterGpuSpanMs = -1;
    bool m_GpuSpansShared = false;
    EventSubscription m_VariantPublishedSub;
    bool m_VariantSubscribed = false;
    uint64_t m_MaterialReadyGeneration = 0;
    RenderServices* m_Services = nullptr;
    SkyEnvironmentSource m_SkyBackground;
    SceneReflectionProbeEnvironmentDesc m_Desc{};
    SceneReflectionProbeEnvironmentDesc m_CaptureDesc{};
    float m_PublishedPosition[3]{};
    const SceneReflectionProbeEnvironmentDesc& CaptureDesc() const
    { return IsBaking() ? m_CaptureDesc : m_Desc; }
    std::string m_ViewDebugName;
    GameEngine::Rendering::CameraId m_CameraId = 0;
    GameEngine::Rendering::ViewId m_ViewId = 0;
    bool m_ViewsAllocated = false;
    CaptureState m_CaptureState = CaptureState::Idle;
    uint32_t m_ArmedFaceMask = 0; // faces the current arm covers (bit = face)
    uint32_t m_SliceFace = 0;     // next face to capture while Slicing
    uint64_t m_SliceDigest = 0;   // digest the current slice cycle bakes FOR
    uint32_t m_CaptureGeneration = 0;
    float m_SecondsSinceSceneBake = 0.0f;
    uint64_t m_LastBaseDigest = 0;

    GameEngine::Rendering::ComputePipelineId m_MipDownsamplePipelineId{};
    GameEngine::Rendering::ComputePipelineId m_DiffusePipelineId{};
    GameEngine::Rendering::ComputePipelineId m_SpecularPipelineId{};
    uint32_t m_PipelineCaptureResolution = 0;
    uint32_t m_PipelinePrefilterMipCount = 0;
    bool m_PipelinesReady = false;
};

} // namespace Engine::Renderer
} // namespace GameEngine
