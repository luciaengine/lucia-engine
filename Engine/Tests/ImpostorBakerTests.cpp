#include "Engine/Rendering/Impostors/ImpostorBaker.h"
#include "Engine/Rendering/RenderServices.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"

#include "Engine/Rendering/PrimitiveGenerator.h"
#include "Engine/Rendering/ViewReadbackUtils.h"
#include "Rendering/Materials/MaterialDocument.h"
#include "Rendering/Core/GPUScene.h"
#include "StagedTestPaths.h"
#include "TestDeviceHelper.h"
#include "Types/Fnv1a.h"

#include <gtest/gtest.h>

namespace GameEngine::Engine::Renderer
{
class ImpostorBakerTests : public ::testing::Test
{
protected:
    void SetUp() override
    {
        Device = CreateVulkanDeviceFast();
        if (!Device)
            GTEST_SKIP() << "No graphics device";
        ASSERT_TRUE(Services.Initialize(Device.get()));
        Persistent = std::make_unique<Rendering::RenderGraph::RGResourcePool>(Device.get());
        Transient = std::make_unique<Rendering::RenderGraph::RGTransientPool>(Device.get());
        Uploads = std::make_unique<Rendering::RenderGraph::RGUploadRing>(Device.get(), 2, 256 * 1024);
        Graph = std::make_unique<Rendering::RenderGraph::RGFrame>(Device.get(), Persistent.get(), Transient.get(), Uploads.get());
        Baker = std::make_unique<ImpostorBaker>();
    }

    void TearDown() override
    {
        if (!Device)
            return;
        Device->WaitForIdle();
        Baker.reset();
        Graph.reset();
        Uploads.reset();
        Transient.reset();
        Persistent.reset();
        Services.Shutdown();
        Device->Shutdown();
    }

    std::unique_ptr<Rendering::IDevice> Device;
    RenderServices Services;
    std::unique_ptr<Rendering::RenderGraph::RGResourcePool> Persistent;
    std::unique_ptr<Rendering::RenderGraph::RGTransientPool> Transient;
    std::unique_ptr<Rendering::RenderGraph::RGUploadRing> Uploads;
    std::unique_ptr<Rendering::RenderGraph::RGFrame> Graph;
    std::unique_ptr<ImpostorBaker> Baker;
};

TEST_F(ImpostorBakerTests, AnAbortedSingleMipBakeRetriesBeforePublishingItsTextures)
{
    using namespace Rendering;
    ImpostorBakeRequest request{};
    request.Key.FramesPerSide = 1;
    request.Key.FrameSize = 16;
    // A blank atlas exercises the real allocation and clear without asynchronous material compilation.
    for (uint32 frame = 1; frame <= 4; ++frame)
    {
        ASSERT_TRUE(Device->BeginFrame());
        Graph->BeginFrame(frame);
        Services.BeginWorldDrawFrame();
        EXPECT_FALSE(Baker->Request(request, Services).Color.IsValid());
        Baker->DeclareFrameStage(*Graph, Services);
        EXPECT_FALSE(Baker->Request(request, Services).Color.IsValid());
        if (frame == 1 || frame == 3)
        {
            Graph->AddPass("Reject atlas frame", 0,
                [](RenderGraph::RGPassBuilder& pass)
                {
                    pass.PreventCulling();
                    pass.Prepare([](RenderGraph::RGPrepareContext&) { return false; });
                }, [](RenderGraph::RGContext&) {});
            const auto submitted = Graph->SubmittedFrameCount();
            Graph->Execute();
            ASSERT_EQ(Graph->SubmittedFrameCount(), submitted);
            ASSERT_EQ(Graph->Stats().PreparationFailures, 1u);
            Baker->OnFrameAbortedRG(*Graph);
            continue;
        }
        Graph->Execute();
        ASSERT_TRUE(Graph->SubmissionToken().IsValid());
        Baker->OnFrameSubmittedRG(*Graph, Graph->SubmissionToken());
        Device->WaitForIdle();
    }
    const auto atlas = Baker->Request(request, Services);
    EXPECT_TRUE(atlas.Color.IsValid());
    EXPECT_TRUE(atlas.NormalDepth.IsValid());
    EXPECT_EQ(Baker->Request(request, Services).Color, atlas.Color);
}

TEST_F(ImpostorBakerTests, ARefusedMeshNeverPublishesEvenWhenAnotherCaptureCompletes)
{
    TestUtils::ScopedTempDir temp{TestUtils::MakeUniqueTempDirectory("impostor-bake")};
    Rendering::MaterialBuildContext context{};
    context.AdapterShaderDir = TestPaths::StagedRenderingShadersDir();
    context.IncludeDirs = {context.AdapterShaderDir};
    context.CacheRoot = temp.Path();
    Services.Materials().SetMaterialBuildContext(context);
    MaterialDocument document{};
    document.materialName = "BakeCube";
    document.lightingModel = "StandardPBR";
    document.surfaceShader = "Surfaces/standard_pbr.glsl";
    const auto material = GUID::Generate();
    ASSERT_NE(Services.Materials().RegisterMaterialFromDocument(material, document), nullptr);
    const auto mesh = Services.GetMeshGPURegistry().RegisterSubmesh({GUID::Generate(), 0}, PrimitiveGenerator::GenerateCube());
    ASSERT_TRUE(mesh.IsValid());
    ImpostorBakeRequest good{};
    good.Key.MeshHash = 1;
    good.Key.FramesPerSide = 1;
    good.Key.FrameSize = 16;
    good.Parts.push_back({mesh, material});
    auto refused = good;
    refused.Key.MeshHash = 2;
    refused.Parts.push_back({Rendering::MeshGPUHandle(0xffff), material});
    auto warm = good;
    warm.Key.MeshHash = 3;
    warm.Key.FramesPerSide = 8;
    warm.Key.FrameSize = 128;
    warm.Key.FilterMips = true;
    bool warmed = false;
    bool sawComplete = false;
    bool sawRefused = false;
    for (uint32 frame = 1; frame <= 100; ++frame)
    {
        ASSERT_TRUE(Device->BeginFrame());
        Graph->BeginFrame(frame);
        Services.BeginWorldDrawFrame();
        Services.GetPerFrameWritePool().BeginFrame(frame);
        Services.Materials().FlushAsyncMaterialCompiles();
        Services.Materials().FinalizeFrameBuffers();
        if (Services.GetGPUScene()->IsDirty())
            Services.GetGPUScene()->FlushGPUBuffers();
        Services.BuildWorldBatchKeys();
        if (!warmed)
            warmed = Baker->Request(warm, Services).Color.IsValid();
        if (warmed)
        {
            sawRefused |= Baker->Request(refused, Services).Color.IsValid();
            sawComplete |= Baker->Request(good, Services).Color.IsValid();
        }
        Baker->DeclareFrameStage(*Graph, Services);
        Graph->Execute();
        Baker->OnFrameSubmittedRG(*Graph, Graph->SubmissionToken());
        Device->WaitForIdle();
        if (warmed)
            sawRefused |= Baker->Request(refused, Services).Color.IsValid();
        if (sawComplete && frame >= 50)
            break;
    }
    EXPECT_FALSE(sawRefused);
    ASSERT_TRUE(sawComplete);
    ASSERT_TRUE(Device->BeginFrame());
    Graph->BeginFrame(101);
    const auto atlas = Baker->Request(good, Services);
    const auto hemisphere = Baker->Request(warm, Services);
    ASSERT_TRUE(hemisphere.HasPoleFrame);
    EXPECT_GT(hemisphere.HighestReadyMip, 0u);
    auto packed = Rendering::RequestDeviceTextureReadbackRG(Device.get(), *Graph, hemisphere.Color,
        Rendering::ResourceState::ShaderResource, "Completed hemisphere cube atlas");
    ASSERT_TRUE(packed);
    auto readback = Rendering::RequestDeviceTextureReadbackRG(Device.get(), *Graph, atlas.Color,
        Rendering::ResourceState::ShaderResource, "Completed cube atlas");
    auto relief = Rendering::RequestDeviceTextureReadbackRG(Device.get(), *Graph, atlas.NormalDepth,
        Rendering::ResourceState::ShaderResource, "Completed cube normal and depth");
    ASSERT_TRUE(readback);
    ASSERT_TRUE(relief);
    Graph->Execute();
    Rendering::OnFrameSubmittedReadbacksRG(*Graph, Graph->SubmissionToken());
    Device->WaitForIdle();
    Rendering::ViewReadbackResult image{};
    ASSERT_TRUE(readback->TryGet(image));
    ASSERT_EQ(image.pixels.size(), 16u * 16u * 4u);
    Rendering::ViewReadbackResult packedImage{};
    ASSERT_TRUE(packed->TryGet(packedImage));
    constexpr uint32 kAtlasSide = 1024;
    ASSERT_EQ(packedImage.pixels.size(), kAtlasSide * kAtlasSide * 4u);
    const auto packedAlphaAt = [&packedImage](uint32 column)
    {
        constexpr uint32 kRowCenter = 3u * 128u + 64u;
        return packedImage.pixels[4u * (kRowCenter * kAtlasSide + column) + 3u];
    };
    // All four view centers contain cube data, with cleared gaps between their rectangles.
    for (uint32 center : {432u, 528u, 624u, 720u})
        EXPECT_NE(packedAlphaAt(center), 0u) << center;
    for (uint32 boundary : {480u, 576u, 672u})
        EXPECT_EQ(packedAlphaAt(boundary), 0u) << boundary;

    Rendering::ViewReadbackResult normalDepth{};
    ASSERT_TRUE(relief->TryGet(normalDepth));
    ASSERT_EQ(normalDepth.pixels.size(), 16u * 16u * 8u);
    RecordProperty("ColorHash", std::to_string(Hashing::Fnv1a64(image.pixels.data(), image.pixels.size())));
    RecordProperty("NormalDepthHash", std::to_string(Hashing::Fnv1a64(normalDepth.pixels.data(), normalDepth.pixels.size())));
    uint32 covered = 0;
    for (size_t alpha = 3; alpha < image.pixels.size(); alpha += 4)
        covered += image.pixels[alpha] != 0;
    EXPECT_GT(covered, 0u);
    EXPECT_LT(covered, 256u);
    RecordProperty("CoveredCubeTexels", static_cast<int>(covered));
}
} // namespace GameEngine::Engine::Renderer
