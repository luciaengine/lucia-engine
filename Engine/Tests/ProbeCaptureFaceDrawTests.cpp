// What a reflection probe capture face draws: the Forward+ lit variant with one light cluster that holds
// the local lights in reach of the face (AddProbeCaptureWorldPass), into a target format that no other
// view uses. Part of ReflectionProbeTests.

#include "AssetCore/GUID.h"
#include "Assets/ModelAsset.h"
#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/MaterialSystem.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "Engine/Rendering/ProbeCaptureWorldPass.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/SceneReflectionProbeEnvironmentSource.h"
#include "Engine/Rendering/ViewReadbackUtils.h"
#include "Engine/Rendering/WorldDrawTypes.h"
#include "Events/Event.h"
#include "Mathematics/HalfFloat.h"
#include "Rendering/Camera/CameraTypes.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/GPUInstanceDepthClass.h"
#include "Rendering/Core/GPUInstanceWorldKey.h"
#include "Rendering/Core/GPUScene.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Core/RenderGraph/RGResourcePool.h"
#include "Rendering/Core/RenderGraph/RGTransientPool.h"
#include "Rendering/Core/RenderGraph/RGUploadRing.h"
#include "Rendering/Core/ReverseZ.h"
#include "Rendering/Materials/MaterialBuildContext.h"
#include "Rendering/Materials/MaterialDocument.h"
#include "StagedTestPaths.h"
#include "TestCameraEye.h"
#include "TestDeviceHelper.h"
#include "Types/Color.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <optional>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Engine::Renderer;
using namespace GameEngine::Rendering;

namespace
{
constexpr uint64_t kWorldId = 7u;
constexpr uint32_t kTargetSize = 8u;
constexpr float kSurfaceDepth = 0.5f;
// The variant compiles off the frame thread; the surface draws a few frames after the first request.
constexpr int kMaxFrames = 32;
// The emission of the surface in scene-linear units: color x luminance / 203.
constexpr float kEmissionLuminance = 101.5f;
constexpr ColorLinear kEmissionColor{1.0f, 0.5f, 0.25f};
constexpr ColorLinear kEmission{0.5f, 0.25f, 0.125f};
// The difference a test accepts between a texel channel and its expected value.
constexpr float kChannelTolerance = 0.01f;

struct FramePools
{
    RenderGraph::RGResourcePool Persistent;
    RenderGraph::RGTransientPool Transient;
    RenderGraph::RGUploadRing Ring;
    explicit FramePools(IDevice* device) : Persistent(device), Transient(device), Ring(device, 2, 65536) {}
};

TextureDesc TargetDesc(TextureFormat format, uint32_t usage)
{
    TextureDesc desc{};
    desc.width = kTargetSize;
    desc.height = kTargetSize;
    desc.depth = 1;
    desc.mipLevels = 1;
    desc.arrayLayers = 1;
    desc.sampleCount = 1;
    desc.format = static_cast<uint32_t>(format);
    desc.usage = usage;
    return desc;
}

// A rectangle that fills the view. Its normal points at the camera, which looks down +Z.
Mesh MakeSurfaceMesh()
{
    Mesh mesh{};
    mesh.Name = "Surface";
    constexpr std::array<std::array<float, 2>, 4> kCorners{{{-1.0f, -1.0f}, {1.0f, -1.0f}, {1.0f, 1.0f}, {-1.0f, 1.0f}}};
    for (const auto& corner : kCorners)
    {
        Vertex vertex{};
        vertex.Position[0] = corner[0];
        vertex.Position[1] = corner[1];
        vertex.Position[2] = kSurfaceDepth;
        vertex.Normal[2] = -1.0f;
        vertex.Tangent[0] = 1.0f;
        vertex.TexCoords[0] = corner[0] * 0.5f + 0.5f;
        vertex.TexCoords[1] = corner[1] * 0.5f + 0.5f;
        mesh.Vertices.push_back(vertex);
    }
    mesh.Indices = {0u, 2u, 1u, 0u, 3u, 2u};
    mesh.MinBounds[0] = mesh.MinBounds[1] = -1.0f;
    mesh.MaxBounds[0] = mesh.MaxBounds[1] = 1.0f;
    mesh.MinBounds[2] = mesh.MaxBounds[2] = kSurfaceDepth;
    return mesh;
}

// A rough white surface with an emission, on the standard surface shader.
MaterialDocument EmissiveDocument()
{
    MaterialDocument doc{};
    doc.materialName = "EmissiveSurface";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "Surfaces/standard_pbr.glsl";
    doc.alphaMode = MaterialAlphaMode::Opaque;
    doc.doubleSided = true;
    doc.properties["baseColor"] = std::vector<float>{0.8f, 0.8f, 0.8f, 1.0f};
    doc.properties["roughness"] = 1.0f;
    doc.properties["metallic"] = 0.0f;
    doc.properties["emissive"] = std::vector<float>{kEmissionColor.r, kEmissionColor.g, kEmissionColor.b};
    doc.properties["emissionLuminance"] = kEmissionLuminance;
    return doc;
}

// A point light between the camera and the surface, at `z` meters on the Z axis.
ExtractedLight PointLightAt(double z)
{
    ExtractedLight light{};
    light.type = Components::LightType::Point;
    light.positionWS = Mathematics::WorldPosition{0.0, 0.0, z};
    light.range = 6.0f;
    light.intensity = 40.0f;
    light.SortId = 1;
    return light;
}

ExtractedLight PointLightNearTheSurface()
{
    return PointLightAt(0.2);
}

// A point light whose reach does not meet the face frustum: the world has a light list but the face
// gets no local light.
ExtractedLight PointLightOutOfReach()
{
    return PointLightAt(-20.0);
}

// A directional light that shines along `directionZ` (+1 or -1) of the Z axis.
ExtractedLight SunAlongZ(float directionZ)
{
    ExtractedLight light{};
    light.type = Components::LightType::Directional;
    light.directionWS[0] = 0.0f;
    light.directionWS[1] = 0.0f;
    light.directionWS[2] = directionZ;
    light.intensity = 2.0f;
    light.SortId = 2;
    return light;
}

bool IsFinite(const ColorLinear& color)
{
    return std::isfinite(color.r) && std::isfinite(color.g) && std::isfinite(color.b);
}

struct SurfaceDraw
{
    // The RGBA16F texel at the center of the color target, from the first frame in which the surface draws.
    std::optional<ColorLinear> Texel;
    // The frames that skipped the draw after the first VariantPublished event: the variant was in the
    // cache, and the pipeline for the target format was not ready.
    int FramesSkippedAfterTheFirstEvent = 0;
    // The input digest of a probe in the same world, at the last frame that skipped the draw and at the
    // frame that drew.
    uint64_t ProbeDigestAtTheLastSkippedFrame = 0;
    uint64_t ProbeDigestAtTheFrameThatDrew = 0;
};

// Draws the surface each frame, as a capture face does, until it shows at the center of the target.
SurfaceDraw DrawTheSurface(IDevice* device, const std::vector<ExtractedLight>& lights)
{
    SurfaceDraw draw;
    const std::filesystem::path shaderDir = TestPaths::StagedRenderingShadersDir();
    if (!std::filesystem::exists(shaderDir))
    {
        ADD_FAILURE() << "staged shader tree not found: " << shaderDir.string();
        return draw;
    }
    RenderServices rs;
    if (!rs.Initialize(device))
    {
        ADD_FAILURE() << "the render services did not start";
        return draw;
    }
    const TestUtils::ScopedTempDir cacheRoot{TestUtils::MakeUniqueTempDirectory("probe-capture-face")};
    MaterialBuildContext context{};
    context.AdapterShaderDir = shaderDir;
    context.CacheRoot = cacheRoot.Path() / "Shaders";
    context.IncludeDirs = {shaderDir};
    rs.Materials().SetMaterialBuildContext(context);

    const Material* material = rs.Materials().RegisterMaterialFromDocument(GUID::Generate(), EmissiveDocument());
    const Mesh mesh = MakeSurfaceMesh();
    const MeshGPUHandle meshHandle = rs.GetMeshGPURegistry().RegisterSubmesh({GUID::Generate(), 0}, mesh);
    const MeshGPUEntry* meshEntry = rs.GetMeshGPURegistry().Find(meshHandle);
    if (material == nullptr || meshEntry == nullptr)
    {
        ADD_FAILURE() << "the material or the mesh did not register";
        rs.Shutdown();
        return draw;
    }

    const CameraId cameraId = rs.Views().AllocateCamera("ProbeCaptureFace.Camera");
    CameraData camera{};
    camera.view = Mathematics::Matrix4x4::Identity();
    camera.proj = Mathematics::Matrix4x4::Identity();
    camera.viewProj = Mathematics::Matrix4x4::Identity();
    rs.Views().SetCameraData(cameraId, ::GameEngine::Tests::TestCameraWithEye(camera), {0.1f, 1000.0f});
    const ViewId viewId = rs.Views().AllocateView("ProbeCaptureFace.View", cameraId);
    rs.Views().SetViewRenderLayerMask(viewId, RenderLayerMask::Default());
    rs.Views().SetViewWorldId(viewId, kWorldId);
    ViewClearConfig clear{};
    clear.clearColor = true;
    // Alpha 0: the opaque surface writes alpha 1, so the texel shows whether the surface drew.
    clear.clearColorValue = ColorLinear(0.0f, 0.0f, 0.0f, 0.0f);
    clear.clearDepth = true;
    clear.clearDepthValue = ReverseZ::kClearFar;
    rs.Views().SetViewTargets(viewId, 0, 0, 0, clear);

    GPUInstance instance{};
    instance.transform = Mathematics::Matrix4x4::Identity();
    instance.prevTransform = Mathematics::Matrix4x4::Identity();
    instance.normalMatrixCol0 = Mathematics::Vector3(1.0f, 0.0f, 0.0f);
    instance.normalMatrixCol1 = Mathematics::Vector4(0.0f, 1.0f, 0.0f, 0.0f);
    instance.normalMatrixCol2 = Mathematics::Vector4(0.0f, 0.0f, 1.0f, 0.0f);
    instance.renderLayerMask = RenderLayerMask::Default().Bits;
    instance.meshIndex = meshEntry->gpuMeshIndex;
    instance.materialIndex = material->GetGpuSceneMaterialIndex();
    instance.flags = DepthClassInstanceFlagBits(rs.Materials().GetMaterialDepthClass(instance.materialIndex)) |
                     (PackWorldKey16(kWorldId) << 16);
    instance.boundingCenter = Mathematics::Vector3(0.0f, 0.0f, kSurfaceDepth);
    instance.boundingRadius = 1.5f;
    std::array<WorldSubmissionRecord, 1> submissions{};
    submissions[0].viewId = viewId;
    submissions[0].meshHandle = meshHandle;
    submissions[0].material = material;
    submissions[0].instanceIndex = rs.GetGPUScene()->AddInstance(instance);
    submissions[0].renderLayerMask = RenderLayerMask::Default();

    {
        int events = 0;
        const EventSubscription published =
            rs.Materials().Variants().VariantPublished.Subscribe([&events](const GUID&) { ++events; });
        SceneReflectionProbeEnvironmentSource probe(rs);
        SceneReflectionProbeEnvironmentDesc probeDesc{};
        probeDesc.SourceKey = 1;
        probeDesc.WorldId = kWorldId;
        probeDesc.UpdateMode = SceneReflectionProbeUpdateMode::Once;
        probe.SetProbe(probeDesc);
        FramePools pools(device);
        RenderGraph::RGFrame frame(device, &pools.Persistent, &pools.Transient, &pools.Ring);
        for (int frameIndex = 0; frameIndex < kMaxFrames && !draw.Texel; ++frameIndex)
        {
            frame.BeginFrame(static_cast<uint64_t>(frameIndex));
            rs.Materials().Variants().SubmitPendingVariantCompiles();
            rs.Materials().Variants().ApplyPendingVariantPublishes();
            rs.Materials().Variants().PruneSettledConcreteWarmWaits();
            rs.BeginWorldDrawFrame();
            rs.GetPerFrameWritePool().BeginFrame(static_cast<uint32_t>(frameIndex));
            for (const ExtractedLight& light : lights)
                rs.SubmitLight(kWorldId, light);
            rs.FinalizeWorldLights(kWorldId);
            rs.SubmitWorldSubmissions(std::span<const WorldSubmissionRecord>(submissions));
            rs.BuildWorldBatchKeys();
            rs.Materials().FinalizeFrameBuffers();
            if (rs.GetGPUScene()->IsDirty())
                rs.GetGPUScene()->FlushGPUBuffers();
            rs.WriteViewLightBuffer(viewId);

            const RenderGraph::RGTexture color = frame.ImportPersistentTexture(
                "ProbeCaptureFace.Color",
                TargetDesc(TextureFormat::R16G16B16A16_FLOAT,
                           static_cast<uint32_t>(TextureUsage::RenderTarget | TextureUsage::ShaderResource)));
            const RenderGraph::RGTexture depth = frame.ImportPersistentTexture(
                "ProbeCaptureFace.Depth",
                TargetDesc(TextureFormat::D32_FLOAT,
                           static_cast<uint32_t>(TextureUsage::DepthStencil | TextureUsage::ShaderResource)));
            rs.ScheduleGpuSkinningAndRetarget(frame);
            rs.ScheduleViewCullingDispatches(frame, 1.0f / 60.0f);
            rs.ScheduleWorldBucketerDispatches(frame);
            RenderServices::WorldPassTargetsRG targets{};
            targets.Color = color;
            targets.Depth = depth;
            if (!AddProbeCaptureWorldPass(rs, frame, viewId, targets, GPUDrawStreamBuilder::kCascadeIndexNone)
                     .Pass.IsValid())
            {
                ADD_FAILURE() << "the world pass was not declared";
                break;
            }
            frame.MarkOutput(color);
            const auto ticket = RequestTextureRegionReadbackRG(device, frame, color, kTargetSize / 2, kTargetSize / 2,
                                                               1, 1, "ProbeCaptureFace.Center");
            frame.Execute();
            OnFrameSubmittedReadbacksRG(frame, frame.SubmissionToken());
            device->WaitForIdle();

            ViewReadbackResult result{};
            if (!ticket || !ticket->TryGet(result) || result.pixels.size() != 4 * sizeof(uint16_t))
            {
                ADD_FAILURE() << "the center texel readback did not resolve";
                break;
            }
            std::array<uint16_t, 4> halves{};
            std::memcpy(halves.data(), result.pixels.data(), sizeof(halves));
            if (Mathematics::HalfToFloat(halves[3]) == 1.0f)
            {
                draw.Texel = ColorLinear(Mathematics::HalfToFloat(halves[0]), Mathematics::HalfToFloat(halves[1]),
                                         Mathematics::HalfToFloat(halves[2]), 1.0f);
                draw.ProbeDigestAtTheFrameThatDrew = probe.InputDigest();
            }
            else
            {
                draw.FramesSkippedAfterTheFirstEvent += events > 0 ? 1 : 0;
                draw.ProbeDigestAtTheLastSkippedFrame = probe.InputDigest();
            }
        }
    }
    rs.Shutdown();
    return draw;
}
} // namespace

// The primary direction in LightUBO is zero when the world has no directional light. The surface then
// shows its emission; a color that is not a number makes the whole capture black.
TEST(ProbeCaptureFaceDraw, AnEmissiveSurfaceKeepsItsEmissionInAWorldWithNoDirectionalLight)
{
    const auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No rendering device available";
    const SurfaceDraw draw = DrawTheSurface(device.get(), {PointLightOutOfReach()});
    ASSERT_TRUE(draw.Texel.has_value()) << "the surface did not draw in " << kMaxFrames << " frames";
    const ColorLinear texel = *draw.Texel;
    ASSERT_TRUE(IsFinite(texel)) << "the color is not a number: " << texel.r << ", " << texel.g << ", " << texel.b;
    EXPECT_NEAR(texel.r, kEmission.r, kChannelTolerance);
    EXPECT_NEAR(texel.g, kEmission.g, kChannelTolerance);
    EXPECT_NEAR(texel.b, kEmission.b, kChannelTolerance);
}

// The double-sided surface faces the light from one side of the Z axis only: a light from that side
// adds its light to the emission, a light from the other side adds none.
TEST(ProbeCaptureFaceDraw, ADirectionalLightAddsItsLightToTheEmission)
{
    const auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No rendering device available";
    float brightest = 0.0f;
    for (const float directionZ : {1.0f, -1.0f})
    {
        const SurfaceDraw draw = DrawTheSurface(device.get(), {PointLightOutOfReach(), SunAlongZ(directionZ)});
        ASSERT_TRUE(draw.Texel.has_value()) << "the surface did not draw in " << kMaxFrames << " frames";
        const ColorLinear texel = *draw.Texel;
        ASSERT_TRUE(IsFinite(texel)) << "the color is not a number: " << texel.r << ", " << texel.g << ", " << texel.b;
        EXPECT_GE(texel.r, kEmission.r - kChannelTolerance);
        EXPECT_GE(texel.g, kEmission.g - kChannelTolerance);
        EXPECT_GE(texel.b, kEmission.b - kChannelTolerance);
        brightest = std::max(brightest, texel.r - kEmission.r);
    }
    EXPECT_GT(brightest, 0.1f) << "no directional light reached the surface";
}

// A capture is the first user of its target format, so its draw is skipped a second time: after the
// variant is in the cache, until the pipeline for that format is ready. A probe keeps its capture, so its
// input digest must move when that pipeline is ready; an equal digest keeps the cube without geometry.
TEST(ProbeCaptureFaceDraw, TheInputDigestOfAProbeMovesWhenASkippedDrawCanDraw)
{
    const auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No rendering device available";
    const SurfaceDraw draw = DrawTheSurface(device.get(), {PointLightOutOfReach()});
    ASSERT_TRUE(draw.Texel.has_value()) << "the surface did not draw in " << kMaxFrames << " frames";
    ASSERT_GE(draw.FramesSkippedAfterTheFirstEvent, 1)
        << "no draw waited on the pipeline for the target format: this test does not see the second skip";
    ASSERT_NE(draw.ProbeDigestAtTheLastSkippedFrame, 0u) << "the probe has no active content";
    EXPECT_NE(draw.ProbeDigestAtTheFrameThatDrew, draw.ProbeDigestAtTheLastSkippedFrame)
        << "the probe keeps the capture that misses the skipped draw";
}

// A point light between the camera and the surface lights the capture face: the face cluster holds it.
TEST(ProbeCaptureFaceDraw, APointLightInReachAddsItsLightToTheEmission)
{
    const auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No rendering device available";
    const SurfaceDraw draw = DrawTheSurface(device.get(), {PointLightNearTheSurface()});
    ASSERT_TRUE(draw.Texel.has_value()) << "the surface did not draw in " << kMaxFrames << " frames";
    const ColorLinear texel = *draw.Texel;
    ASSERT_TRUE(IsFinite(texel)) << "the color is not a number: " << texel.r << ", " << texel.g << ", " << texel.b;
    EXPECT_GT(texel.r - kEmission.r, 0.1f) << "the point light did not reach the surface";
    EXPECT_GE(texel.g, kEmission.g - kChannelTolerance);
    EXPECT_GE(texel.b, kEmission.b - kChannelTolerance);
}
