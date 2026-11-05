#include "Assets/ModelAsset.h"
#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/ViewReadbackUtils.h"
#include "Rendering/Core/GPUScene.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Materials/MaterialBuildService.h"
#include "Rendering/Materials/MaterialDocument.h"
#include "StagedTestPaths.h"
#include "TestTempDir.h"

#include <array>
#include <fstream>
#include <gtest/gtest.h>

using namespace GameEngine;
using namespace GameEngine::Engine::Renderer;
using namespace GameEngine::Rendering;

namespace
{
Material* MakeBlendSurface(RenderServices& services, const std::filesystem::path& root,
                           const char* name, const char* rgb)
{
    const std::string file = std::string(name) + ".glsl";
    std::ofstream(root / file)
        << "SurfaceOutput EvaluateSurface(SurfaceInput sIn) { SurfaceOutput o = DefaultSurfaceOutput();"
        << "o.baseColor = vec3(" << rgb << "); o.opacity = 0.5; return o; }\n";
    MaterialDocument document{};
    document.materialName = name;
    document.lightingModel = "Unlit";
    document.surfaceShader = file;
    document.alphaMode = MaterialAlphaMode::Blend;
    document.doubleSided = true;
    document.zTest = false;
    return services.Materials().RegisterMaterialFromDocument(GUID::Generate(), document);
}

class SortedTransparentPixel : public ::testing::TestWithParam<GraphicsAPI>
{
  protected:
    void SetUp() override
    {
        DeviceDesc description{};
        description.preferredAPI = GetParam();
        description.enableSwapchain = false;
        description.enableDebugLayer = false;
        description.enableDynamicRendering = true;
        description.pipelineCacheDirectory = (m_TempDirectory.Path() / "Pipelines").string();
        m_Device = DeviceFactory::CreateDevice(description);
        if (!m_Device || !m_Device->Initialize(description))
        {
            m_Device.reset();
            GTEST_SKIP() << "Requested graphics device unavailable";
        }
        ASSERT_EQ(m_Device->GetAPI(), GetParam());
        ASSERT_TRUE(m_Services.Initialize(m_Device.get()));
        MaterialBuildContext context{};
        context.AdapterShaderDir = TestPaths::StagedRenderingShadersDir();
        ASSERT_TRUE(std::filesystem::is_directory(context.AdapterShaderDir));
        context.CacheRoot = m_TempDirectory.Path() / "Cache";
        context.IncludeDirs = {context.AdapterShaderDir};
        context.ProjectRoots = {m_TempDirectory.Path()};
        context.AssetSourceRoots = {{"project", m_TempDirectory.Path()}};
        m_Services.Materials().SetMaterialBuildContext(context);
    }

    void TearDown() override
    {
        if (m_Device)
        {
            m_Services.Shutdown();
            m_Device->Shutdown();
        }
    }

    TestUtils::ScopedTempDir m_TempDirectory{TestUtils::MakeUniqueTempDirectory("transparent-pixel")};
    std::unique_ptr<IDevice> m_Device;
    RenderServices m_Services;
};
} // namespace

TEST_P(SortedTransparentPixel, MeshOnlyInterleavedSurfacesCompositeBackToFront)
{
    auto* red = MakeBlendSurface(m_Services, m_TempDirectory.Path(), "Red", "1,0,0");
    auto* blue = MakeBlendSurface(m_Services, m_TempDirectory.Path(), "Blue", "0,0,1");
    ASSERT_TRUE(red && blue);

    const auto cameraId = m_Services.Views().AllocateCamera("BlendCamera");
    CameraData camera{};
    for (int i = 0; i < 16; i += 5)
        camera.view[i] = camera.proj[i] = camera.viewProj[i] = 1.0f;
    m_Services.Views().SetCameraData(cameraId, camera);
    const auto view = m_Services.Views().AllocateView("BlendView", cameraId);
    m_Services.Views().SetViewRenderLayerMask(view, 1u);

    Mesh triangle{};
    triangle.Vertices.resize(3);
    triangle.Vertices[0].Position[0] = -1;
    triangle.Vertices[0].Position[1] = -1;
    triangle.Vertices[1].Position[0] = 1;
    triangle.Vertices[1].Position[1] = -1;
    triangle.Vertices[2].Position[1] = 1;
    triangle.Indices = {0, 1, 2};
    const auto mesh = m_Services.GetMeshGPURegistry().RegisterSubmesh({GUID::Generate(), 0}, triangle);
    ASSERT_TRUE(mesh.IsValid());

    m_Services.BeginWorldDrawFrame();
    std::array<WorldSubmissionRecord, 3> submissions{};
    const Material* materials[] = {red, red, blue};
    constexpr float kDepths[] = {0.75f, 0.25f, 0.5f};
    for (size_t submissionIndex = 0; submissionIndex < submissions.size(); ++submissionIndex)
    {
        GPUInstance instance{};
        instance.transform = Matrix4x4::Translation(Vector3(0, 0, kDepths[submissionIndex]));
        instance.boundingCenter = {0, 0, kDepths[submissionIndex]};
        instance.boundingRadius = 1;
        instance.materialIndex = materials[submissionIndex]->GetGpuSceneMaterialIndex();
        submissions[submissionIndex].viewId = view;
        submissions[submissionIndex].material = materials[submissionIndex];
        submissions[submissionIndex].meshHandle = mesh;
        submissions[submissionIndex].renderLayerMask = 1;
        submissions[submissionIndex].instanceIndex = m_Services.GetGPUScene()->AddInstance(instance);
    }
    m_Services.SubmitWorldSubmissions(submissions);
    m_Services.BuildWorldBatchKeys();
    m_Services.GetGPUScene()->BeginFrame();
    m_Services.Materials().FinalizeFrameBuffers();
    m_Services.BuildSortedTransparentForView(view);
    ASSERT_TRUE(m_Services.IsSortedTransparentActive(view));

    RenderGraph::RGResourcePool persistent(m_Device.get());
    RenderGraph::RGTransientPool transients(m_Device.get());
    RenderGraph::RGUploadRing uploads(m_Device.get(), 2, 65536);
    RenderGraph::RGFrame frame(m_Device.get(), &persistent, &transients, &uploads);
    ViewReadbackResult result{};
    // Publish the exact pass variant before sampling the settled frame.
    for (uint32_t iteration = 0; iteration < 3; ++iteration)
    {
        m_Services.Materials().Variants().SubmitPendingVariantCompiles();
        m_Services.Materials().Variants().ApplyPendingVariantPublishes();
        m_Services.Materials().Variants().PruneSettledConcreteWarmWaits();
        frame.BeginFrame(iteration);
        TextureDesc description{};
        description.width = description.height = 8;
        description.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
        description.usage = static_cast<uint32_t>(TextureUsage::RenderTarget);
        const auto color = frame.CreateTexture("Blend.Color", description);
        frame.AddPass("Clear", 0, [color](RenderGraph::RGPassBuilder& pass)
                      { pass.AttachColor(0, color, {.Load = RenderGraph::RGLoadOp::Clear}); },
                      [](RenderGraph::RGContext&) {});
        RenderServices::WorldPassTargetsRG targets{};
        targets.Color = color;
        m_Services.AddSortedTransparentDrainForView(frame, view, targets, MaterialKeyword::Instanced);
        const auto ticket = RequestTextureRegionReadbackRG(m_Device.get(), frame, color, 4, 4, 1, 1, "Blend.Center");
        ASSERT_TRUE(ticket);
        frame.Execute();
        OnFrameSubmittedReadbacksRG(frame, frame.SubmissionToken());
        m_Device->WaitForIdle();
        ASSERT_TRUE(ticket->TryGet(result));
    }
    ASSERT_EQ(result.pixels.size(), 4u);
    // Red over blue over red at half opacity on black is (5/8, 0, 1/4).
    // Drawing both reds together would instead leave (3/8, 0, 1/2).
    EXPECT_NEAR(result.pixels[0], 159, 2);
    EXPECT_EQ(result.pixels[1], 0);
    EXPECT_NEAR(result.pixels[2], 64, 2);

    // A view that reuses a device ring slot must release the previous frame
    // records before deciding which draws belong in the opaque pass.
    m_Services.BeginWorldDrawFrame();
    EXPECT_FALSE(m_Services.IsSortedTransparentActive(view));
    m_Services.BuildSortedTransparentForView(view);
    EXPECT_FALSE(m_Services.IsSortedTransparentActive(view));
}

TEST_P(SortedTransparentPixel, DepthTestOverrideChangesFullProfileRunCompatibility)
{
    MaterialDocument document{};
    document.materialName = "Depth state";
    document.lightingModel = "Unlit";
    document.surfaceShader = "Surfaces/unlit_solid.glsl";
    document.alphaMode = MaterialAlphaMode::Blend;
    const auto* normal = m_Services.Materials().RegisterMaterialFromDocument(GUID::Generate(), document);
    document.zTest = false;
    const auto* overlay = m_Services.Materials().RegisterMaterialFromDocument(GUID::Generate(), document);
    document.zTest = true;
    const auto* explicitTest = m_Services.Materials().RegisterMaterialFromDocument(GUID::Generate(), document);
    ASSERT_TRUE(normal && overlay && explicitTest);
    EXPECT_NE(MakeSortedTransparentRunMaterialKey(*normal), MakeSortedTransparentRunMaterialKey(*overlay));
    EXPECT_EQ(MakeSortedTransparentRunMaterialKey(*normal), MakeSortedTransparentRunMaterialKey(*explicitTest));
}

namespace
{
constexpr GraphicsAPI kBackends[]{
    GraphicsAPI::Vulkan,
#if defined(RENDERING_HAS_METAL)
    GraphicsAPI::Metal,
#endif
};
}
INSTANTIATE_TEST_SUITE_P(Backend, SortedTransparentPixel, ::testing::ValuesIn(kBackends),
                         [](const ::testing::TestParamInfo<GraphicsAPI>& info)
                         { return info.param == GraphicsAPI::Metal ? "Metal" : "Vulkan"; });
