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
    MaterialDocument doc{};
    doc.materialName = name;
    doc.lightingModel = "Unlit";
    doc.surfaceShader = file;
    doc.alphaMode = MaterialAlphaMode::Blend;
    doc.doubleSided = true;
    doc.zTest = false;
    return services.Materials().RegisterMaterialFromDocument(GUID::Generate(), doc);
}

class SortedTransparentPixel : public ::testing::TestWithParam<GraphicsAPI>
{
protected:
    void SetUp() override
    {
        DeviceDesc desc{};
        desc.preferredAPI = GetParam();
        desc.enableSwapchain = false;
        desc.enableDebugLayer = false;
        desc.enableDynamicRendering = true;
        desc.pipelineCacheDirectory = (temp.Path() / "Pipelines").string();
        device = DeviceFactory::CreateDevice(desc);
        if (!device || !device->Initialize(desc))
        {
            device.reset();
            GTEST_SKIP() << "Requested graphics device unavailable";
        }
        ASSERT_EQ(device->GetAPI(), GetParam());
        ASSERT_TRUE(services.Initialize(device.get()));
        MaterialBuildContext context{};
        context.AdapterShaderDir = TestPaths::StagedRenderingShadersDir();
        ASSERT_TRUE(std::filesystem::is_directory(context.AdapterShaderDir));
        context.CacheRoot = temp.Path() / "Cache";
        context.IncludeDirs = {context.AdapterShaderDir};
        context.ProjectRoots = {temp.Path()};
        context.AssetSourceRoots = {{"project", temp.Path()}};
        services.Materials().SetMaterialBuildContext(context);
    }

    void TearDown() override
    {
        if (device)
        {
            services.Shutdown();
            device->Shutdown();
        }
    }

    TestUtils::ScopedTempDir temp{TestUtils::MakeUniqueTempDirectory("transparent-pixel")};
    std::unique_ptr<IDevice> device;
    RenderServices services;
};
} // namespace

TEST_P(SortedTransparentPixel, MeshOnlyInterleavedSurfacesCompositeBackToFront)
{
    auto* red = MakeBlendSurface(services, temp.Path(), "Red", "1,0,0");
    auto* blue = MakeBlendSurface(services, temp.Path(), "Blue", "0,0,1");
    ASSERT_TRUE(red && blue);

    const auto cam = services.Views().AllocateCamera("BlendCamera");
    CameraData camera{};
    for (int i = 0; i < 16; i += 5)
        camera.view[i] = camera.proj[i] = camera.viewProj[i] = 1.0f;
    services.Views().SetCameraData(cam, camera);
    const auto view = services.Views().AllocateView("BlendView", cam);
    services.Views().SetViewRenderLayerMask(view, 1u);

    Mesh triangle{};
    triangle.Vertices.resize(3);
    triangle.Vertices[0].Position[0] = -1;
    triangle.Vertices[0].Position[1] = -1;
    triangle.Vertices[1].Position[0] = 1;
    triangle.Vertices[1].Position[1] = -1;
    triangle.Vertices[2].Position[1] = 1;
    triangle.Indices = {0, 1, 2};
    const auto mesh = services.GetMeshGPURegistry().RegisterSubmesh({GUID::Generate(), 0}, triangle);
    ASSERT_TRUE(mesh.IsValid());

    services.BeginWorldDrawFrame();
    std::array<WorldSubmissionRecord, 3> submissions{};
    const Material* materials[] = {red, red, blue};
    constexpr float depths[] = {0.75f, 0.25f, 0.5f};
    for (size_t i = 0; i < submissions.size(); ++i)
    {
        GPUInstance instance{};
        instance.transform = Matrix4x4::Translation(Vector3(0, 0, depths[i]));
        instance.boundingCenter = {0, 0, depths[i]};
        instance.boundingRadius = 1;
        instance.materialIndex = materials[i]->GetGpuSceneMaterialIndex();
        submissions[i].viewId = view;
        submissions[i].material = materials[i];
        submissions[i].meshHandle = mesh;
        submissions[i].renderLayerMask = 1;
        submissions[i].instanceIndex = services.GetGPUScene()->AddInstance(instance);
    }
    services.SubmitWorldSubmissions(submissions);
    services.BuildWorldBatchKeys();
    services.GetGPUScene()->BeginFrame();
    services.Materials().FinalizeFrameBuffers();
    services.BuildSortedTransparentForView(view);
    ASSERT_TRUE(services.IsSortedTransparentActive(view));

    RenderGraph::RGResourcePool persistent(device.get());
    RenderGraph::RGTransientPool transients(device.get());
    RenderGraph::RGUploadRing uploads(device.get(), 2, 65536);
    RenderGraph::RGFrame frame(device.get(), &persistent, &transients, &uploads);
    ViewReadbackResult result{};
    // A cold material requests its exact pass variant on first use. Drive the usual
    // publish boundary before sampling the settled frame.
    for (uint32_t iteration = 0; iteration < 3; ++iteration)
    {
        services.Materials().Variants().SubmitPendingVariantCompiles();
        services.Materials().Variants().ApplyPendingVariantPublishes();
        services.Materials().Variants().PruneSettledConcreteWarmWaits();
        frame.BeginFrame(iteration);
        TextureDesc desc{};
        desc.width = desc.height = 8;
        desc.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
        desc.usage = static_cast<uint32_t>(TextureUsage::RenderTarget);
        const auto color = frame.CreateTexture("Blend.Color", desc);
        frame.AddPass("Clear", 0, [&](RenderGraph::RGPassBuilder& p)
                      { p.AttachColor(0, color, {.Load = RenderGraph::RGLoadOp::Clear}); },
                      [](RenderGraph::RGContext&) {});
        RenderServices::WorldPassTargetsRG targets{};
        targets.Color = color;
        services.AddSortedTransparentDrainForView(frame, view, targets, MaterialKeyword::Instanced);
        const auto ticket = RequestTextureRegionReadbackRG(device.get(), frame, color, 4, 4, 1, 1, "Blend.Center");
        ASSERT_TRUE(ticket);
        frame.Execute();
        OnFrameSubmittedReadbacksRG(frame, frame.SubmissionToken());
        device->WaitForIdle();
        ASSERT_TRUE(ticket->TryGet(result));
    }
    ASSERT_EQ(result.pixels.size(), 4u);
    // Red over blue over red at half opacity on black is (5/8, 0, 1/4).
    // Drawing both reds together would instead leave (3/8, 0, 1/2).
    EXPECT_NEAR(result.pixels[0], 159, 2);
    EXPECT_EQ(result.pixels[1], 0);
    EXPECT_NEAR(result.pixels[2], 64, 2);

    // An on-demand view can return on the same device ring slot in a later
    // app frame. Its old record list must no longer own the opaque peel.
    services.BeginWorldDrawFrame();
    EXPECT_FALSE(services.IsSortedTransparentActive(view));
    services.BuildSortedTransparentForView(view);
    EXPECT_FALSE(services.IsSortedTransparentActive(view));
}

TEST_P(SortedTransparentPixel, DepthTestOverrideChangesFullProfileRunCompatibility)
{
    MaterialDocument doc{};
    doc.materialName = "Depth state";
    doc.lightingModel = "Unlit";
    doc.surfaceShader = "Surfaces/unlit_solid.glsl";
    doc.alphaMode = MaterialAlphaMode::Blend;
    const auto* normal = services.Materials().RegisterMaterialFromDocument(GUID::Generate(), doc);
    doc.zTest = false;
    const auto* overlay = services.Materials().RegisterMaterialFromDocument(GUID::Generate(), doc);
    doc.zTest = true;
    const auto* explicitTest = services.Materials().RegisterMaterialFromDocument(GUID::Generate(), doc);
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
