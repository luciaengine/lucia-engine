#include <gtest/gtest.h>
#include <Rendering/Core/Device.h>
#include <Rendering/Core/CommandList.h>
#include "Rendering/Common/Utils.h"
#include "TestUtils.h"
#include "TestGlslCompile.h"
#include "ScopedEnvVar.h"


using namespace GameEngine::Rendering;

#if RENDERING_HAS_SHADERC
TEST(DynamicRenderingPipelineFormats, OneVulkanMeshWorkgroupWritesTheTarget)
{
    const Tests::ScopedEnvVar headless("GE_HEADLESS_TEST", "1");
    DeviceDesc deviceDesc{};
    deviceDesc.preferredAPI = GraphicsAPI::Vulkan;
    deviceDesc.enableDynamicRendering = true;
    auto device = DeviceFactory::CreateDevice(deviceDesc);
    if (!device || !device->Initialize(deviceDesc) || !device->GetCapabilities().supportsMeshShaders)
        GTEST_SKIP() << "Vulkan mesh shaders unavailable";

    constexpr char kMesh[] = R"(
#version 460
#extension GL_EXT_mesh_shader : require
layout(local_size_x = 1) in;
layout(triangles, max_vertices = 3, max_primitives = 1) out;
void main()
{
    SetMeshOutputsEXT(3, 1);
    gl_MeshVerticesEXT[0].gl_Position = vec4(-0.75, -0.75, 0.5, 1.0);
    gl_MeshVerticesEXT[1].gl_Position = vec4( 0.75, -0.75, 0.5, 1.0);
    gl_MeshVerticesEXT[2].gl_Position = vec4( 0.0,   0.75, 0.5, 1.0);
    gl_PrimitiveTriangleIndicesEXT[0] = uvec3(0, 1, 2);
}
)";
    constexpr char kFragment[] = R"(
#version 450
layout(location = 0) out vec4 color;
void main() { color = vec4(0.0, 1.0, 0.0, 1.0); }
)";
    constexpr uint32_t kExtent = 16;
    constexpr auto kFormat = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
    PipelineDesc pipelineDesc{};
    pipelineDesc.type = PipelineType::Mesh;
    pipelineDesc.meshShader = Tests::CompileGlsl(kMesh, shaderc_mesh_shader, "one-workgroup.mesh");
    pipelineDesc.pixelShader = Tests::CompileGlsl(kFragment, shaderc_fragment_shader, "green.frag");
    ASSERT_FALSE(pipelineDesc.meshShader.empty());
    ASSERT_FALSE(pipelineDesc.pixelShader.empty());
    pipelineDesc.colorAttachmentFormats = {kFormat};
    pipelineDesc.rasterizationState.cullMode = CullModeFlagBits::None;
    pipelineDesc.EnableDepthTest(false);
    pipelineDesc.depthStencilState.depthWriteEnable = false;
    const PipelineHandle pipeline = device->CreatePipeline(pipelineDesc);
    ASSERT_TRUE(pipeline.IsValid());

    TextureDesc targetDesc{};
    targetDesc.width = kExtent;
    targetDesc.height = kExtent;
    targetDesc.format = kFormat;
    targetDesc.usage = static_cast<uint32_t>(TextureUsage::RenderTarget | TextureUsage::TransferSrc);
    const TextureHandle target = device->CreateTexture(targetDesc);
    const BufferHandle readback = device->CreateReadbackBuffer(kExtent * kExtent * 4u, "MeshWorkgroup.Readback");
    ASSERT_TRUE(target.IsValid());
    ASSERT_TRUE(readback.IsValid());

    auto commands = device->CreateCommandList(IDevice::QueueType::Graphics);
    RenderPassDesc pass{};
    pass.colorTargets[0] = target;
    pass.colorTargetCount = 1;
    pass.clearColor[0] = true;
    pass.clearColorValue[0][0] = 1.0f;
    pass.clearColorValue[0][3] = 1.0f;
    commands->Begin();
    commands->BeginRenderPass(pass);
    commands->SetPipeline(pipeline);
    commands->SetViewport(0.0f, 0.0f, kExtent, kExtent);
    commands->SetScissor(0, 0, kExtent, kExtent);
    commands->DrawMeshTasks(1);
    commands->EndRenderPass();
    commands->Barrier(ResourceBarrier::CreateTextureBarrier(target, ResourceState::RenderTarget, ResourceState::CopySource));
    commands->CopyTextureToBuffer(target, readback, kExtent, kExtent);
    commands->End();
    device->ExecuteCommandLists({commands.get()});
    device->WaitForIdle();

    const auto* pixels = static_cast<const uint8_t*>(device->MapBuffer(readback));
    ASSERT_NE(pixels, nullptr);
    const auto* center = pixels + ((kExtent / 2) * kExtent + kExtent / 2) * 4;
    EXPECT_EQ(center[0], 0u);
    EXPECT_EQ(center[1], 255u);
    EXPECT_EQ(pixels[0], 255u);
    EXPECT_EQ(pixels[1], 0u);
    device->UnmapBuffer(readback);
    device->DestroyBuffer(readback);
    device->DestroyTexture(target);
    device->DestroyPipeline(pipeline);
}
#endif

TEST(DynamicRenderingPipelineFormats, OffscreenRGBA8WritesRed)
{
#ifdef _WIN32
    _putenv_s("GE_HEADLESS_TEST", "1");
#else
    setenv("GE_HEADLESS_TEST", "1", 1);
#endif

    // Create device with dynamic rendering enabled (no validation layers required)
    DeviceDesc dd{};
    dd.preferredAPI = GraphicsAPI::Vulkan;
    dd.enableDynamicRendering = true;
    dd.enableDebugLayer = false;
    dd.enableDescriptorValidation = false;

    auto dev = DeviceFactory::CreateDevice(dd);
    ASSERT_TRUE(dev);
    if (!dev->Initialize(dd)) {
        GTEST_SKIP() << "Device init failed";
    }

    // Create a 1x1 source texture filled solid red
    TextureDesc src{}; src.width = 1; src.height = 1; src.mipLevels = 1; src.arrayLayers = 1;
    src.format = (uint32_t)TextureFormat::RGBA8_UNORM;
    src.usage = (uint32_t)(TextureUsage::ShaderResource | TextureUsage::TransferDst);
    src.debugName = "DynDR_Src1x1";
    TextureHandle th = dev->CreateTexture(src);
    ASSERT_TRUE(th.IsValid());

    {
        auto cl = dev->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        const float red[4] = {1.f, 0.f, 0.f, 1.f};
        cl->ClearColorImageSubresource(th, /*mip*/0, /*layer*/0, red);
        // Transition to sample from
        cl->Barrier(ResourceBarrier::CreateTextureBarrier(
            th, ResourceState::CopyDest, ResourceState::ShaderResource, 0, 1, 0, 1));
        cl->End();
        dev->ExecuteCommandLists({ cl.get() });
        dev->WaitForIdle();
    }

    // Create 16x16 destination render target
    TextureDesc dst{}; dst.width = 16; dst.height = 16; dst.mipLevels = 1; dst.arrayLayers = 1;
    dst.format = (uint32_t)TextureFormat::RGBA8_UNORM;
    dst.usage = (uint32_t)(TextureUsage::RenderTarget | TextureUsage::TransferSrc);
    dst.debugName = "DynDR_Dst16x16";
    TextureHandle dth = dev->CreateTexture(dst);
    ASSERT_TRUE(dth.IsValid());

    // Build pipeline (dynamic rendering, explicit color attachment format RGBA8)
    DescriptorSetLayoutDesc setLayout{};
    setLayout.bindings.push_back({0, DescriptorType::CombinedImageSampler, 1, /*frag*/0x10});

    PipelineDesc pd{}; pd.type = PipelineType::Graphics; pd.debugName = "DynDR_Copy";
    pd.vertexShader = GameEngine::Rendering::Tests::ReadSpirvBytes("fullscreen_noinput.vert.spv");
    pd.pixelShader  = GameEngine::Rendering::Tests::ReadSpirvBytes("copy.frag.spv");
    ASSERT_FALSE(pd.vertexShader.empty());
    ASSERT_FALSE(pd.pixelShader.empty());
    pd.EnableDepthTest(false);
    pd.AddDynamicState(DynamicState::Viewport);
    pd.AddDynamicState(DynamicState::Scissor);
    pd.colorAttachmentFormats.push_back((uint32_t)TextureFormat::RGBA8_UNORM);
    pd.descriptorSetLayouts.push_back(setLayout);

    PipelineHandle pipe = dev->CreatePipeline(pd);
    ASSERT_TRUE(pipe.IsValid());

    // Sampler + descriptor set
    SamplerDesc sdef = SamplerDesc::MaterialLinearRepeat("DynDR_Samp");
    SamplerHandle samp = dev->CreateSampler(sdef);
    ASSERT_TRUE(samp.IsValid());

    DescriptorSetDesc dsd{}; dsd.layout = setLayout; dsd.transient = false; dsd.debugName = "DynDR_Set";
    DescriptorSetHandle ds = dev->CreateDescriptorSet(dsd);
    ASSERT_TRUE(ds.IsValid());

    dev->UpdateCombinedImageSamplerBinding(ds, 0, th, samp);

    // Render: sample src into dst
    auto cl = dev->CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();
    RenderPassDesc rp{}; rp.colorTargets[0] = dth; rp.colorTargetCount = 1; rp.clearColor[0] = true; // clear to black first
    cl->BeginRenderPass(rp);
    cl->SetPipeline(pipe);
    cl->SetViewport(0, 0, (float)dst.width, (float)dst.height);
    cl->SetScissor(0, 0, dst.width, dst.height);
    cl->BindDescriptorSet(0, ds, pipe);
    cl->Draw(3, 1);
    cl->EndRenderPass();

    // Prepare readback
    cl->Barrier(ResourceBarrier::CreateTextureBarrier(dth, ResourceState::RenderTarget, ResourceState::CopySource));
    const uint32_t rbSize = dst.width * dst.height * 4u;
    BufferHandle rb = dev->CreateReadbackBuffer(rbSize, "DynDR_RB");
    ASSERT_TRUE(rb.IsValid());
    cl->CopyTextureToBuffer(dth, rb, dst.width, dst.height);
    cl->End();

    dev->ExecuteCommandLists({ cl.get() });
    dev->WaitForIdle();

    // Validate pixels are red (proves pipeline wrote to color 0 under dynamic rendering)
    const uint8_t* data = static_cast<const uint8_t*>(dev->MapBuffer(rb));
    ASSERT_NE(data, nullptr);
    auto idx = [&](uint32_t x, uint32_t y){ return (y*dst.width + x)*4u; };
    for (auto xy : { std::pair<uint32_t,uint32_t>{0,0}, {8,8}, {15,15} }) {
        uint32_t i = idx(xy.first, xy.second);
        EXPECT_GE(data[i+0], 200) << "R at ("<<xy.first<<","<<xy.second<<")";
        EXPECT_LE(data[i+1], 20)  << "G at ("<<xy.first<<","<<xy.second<<")";
        EXPECT_LE(data[i+2], 20)  << "B at ("<<xy.first<<","<<xy.second<<")";
        EXPECT_GE(data[i+3], 200) << "A at ("<<xy.first<<","<<xy.second<<")";
    }

    // Cleanup to keep validation clean (even if disabled here)
    dev->UnmapBuffer(rb);
    dev->DestroyDescriptorSet(ds);
    dev->DestroySampler(samp);
    dev->DestroyPipeline(pipe);
    dev->DestroyBuffer(rb);
    dev->DestroyTexture(dth);
    dev->DestroyTexture(th);
}



#if GTEST_HAS_DEATH_TEST
TEST(DynamicRenderingPipelineFormats, MismatchedFormatsDeath)
{
#ifndef NDEBUG
#ifdef _WIN32
    _putenv_s("GE_HEADLESS_TEST", "1");
#else
    setenv("GE_HEADLESS_TEST", "1", 1);
#endif

    DeviceDesc dd{}; dd.preferredAPI = GraphicsAPI::Vulkan; dd.enableDynamicRendering = true; dd.enableDebugLayer = false; dd.enableDescriptorValidation = false;
    auto dev = DeviceFactory::CreateDevice(dd);
    ASSERT_TRUE(dev);
    if (!dev->Initialize(dd)) { GTEST_SKIP() << "Device init failed"; }

    // Build pipeline expecting RGBA8
    DescriptorSetLayoutDesc setLayout{};
    PipelineDesc pd{}; pd.type = PipelineType::Graphics; pd.debugName = "DynDR_Mismatch";
    pd.vertexShader = GameEngine::Rendering::Tests::ReadSpirvBytes("fullscreen_noinput.vert.spv");
    pd.pixelShader  = GameEngine::Rendering::Tests::ReadSpirvBytes("copy.frag.spv");
    ASSERT_FALSE(pd.vertexShader.empty()); ASSERT_FALSE(pd.pixelShader.empty());
    pd.EnableDepthTest(false);
    pd.AddDynamicState(DynamicState::Viewport);
    pd.AddDynamicState(DynamicState::Scissor);
    pd.colorAttachmentFormats.push_back((uint32_t)TextureFormat::RGBA8_UNORM);
    pd.descriptorSetLayouts.push_back(setLayout);

    PipelineHandle pipe = dev->CreatePipeline(pd);
    ASSERT_TRUE(pipe.IsValid());

    ASSERT_DEATH({
        auto cl = dev->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        TextureDesc dst{}; dst.width = 8; dst.height = 8; dst.mipLevels = 1; dst.arrayLayers = 1;
        dst.format = (uint32_t)TextureFormat::RGBA8_SRGB; // Mismatch with pipeline below
        dst.usage = (uint32_t)(TextureUsage::RenderTarget | TextureUsage::TransferSrc);
        TextureHandle dth_local = dev->CreateTexture(dst);
        RenderPassDesc rp{}; rp.colorTargets[0] = dth_local; rp.colorTargetCount = 1; rp.clearColor[0] = true;
        cl->BeginRenderPass(rp);
        cl->SetPipeline(pipe); // Should trigger assert in debug due to format mismatch
    }, ".*");

    // Cleanup
    dev->DestroyPipeline(pipe);
#else
    GTEST_SKIP() << "Death test only runs in Debug builds";
#endif
}
#endif // GTEST_HAS_DEATH_TEST

#if GTEST_HAS_DEATH_TEST
TEST(DynamicRenderingPipelineFormats, MismatchedAttachmentCountDeath)
{
#ifndef NDEBUG
#ifdef _WIN32
    _putenv_s("GE_HEADLESS_TEST", "1");
#else
    setenv("GE_HEADLESS_TEST", "1", 1);
#endif

    DeviceDesc dd{}; dd.preferredAPI = GraphicsAPI::Vulkan; dd.enableDynamicRendering = true; dd.enableDebugLayer = false; dd.enableDescriptorValidation = false;
    auto dev = DeviceFactory::CreateDevice(dd);
    ASSERT_TRUE(dev);
    if (!dev->Initialize(dd)) { GTEST_SKIP() << "Device init failed"; }

    // Build pipeline expecting TWO color attachments
    PipelineDesc pd{}; pd.type = PipelineType::Graphics; pd.debugName = "DynDR_CountMismatch";
    pd.vertexShader = GameEngine::Rendering::Tests::ReadSpirvBytes("fullscreen_noinput.vert.spv");
    pd.pixelShader  = GameEngine::Rendering::Tests::ReadSpirvBytes("copy.frag.spv");
    ASSERT_FALSE(pd.vertexShader.empty()); ASSERT_FALSE(pd.pixelShader.empty());
    pd.EnableDepthTest(false);
    pd.AddDynamicState(DynamicState::Viewport);
    pd.AddDynamicState(DynamicState::Scissor);
    pd.colorAttachmentFormats.push_back((uint32_t)TextureFormat::RGBA8_UNORM);
    pd.colorAttachmentFormats.push_back((uint32_t)TextureFormat::RGBA8_UNORM);

    PipelineHandle pipe = dev->CreatePipeline(pd);
    ASSERT_TRUE(pipe.IsValid());

    ASSERT_DEATH({
        auto cl = dev->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        TextureDesc dst{}; dst.width = 8; dst.height = 8; dst.mipLevels = 1; dst.arrayLayers = 1;
        dst.format = (uint32_t)TextureFormat::RGBA8_UNORM;
        dst.usage = (uint32_t)(TextureUsage::RenderTarget | TextureUsage::TransferSrc);
        TextureHandle dth_local = dev->CreateTexture(dst);
        RenderPassDesc rp{}; rp.colorTargets[0] = dth_local; rp.colorTargetCount = 1; rp.clearColor[0] = true;
        cl->BeginRenderPass(rp);
        cl->SetPipeline(pipe); // Should trigger assert in debug due to COUNT mismatch
    }, ".*");

    // Cleanup
    dev->DestroyPipeline(pipe);
#else
    GTEST_SKIP() << "Death test only runs in Debug builds";
#endif
}
#endif // GTEST_HAS_DEATH_TEST



TEST(DynamicRenderingPipelineFormats, MissingFormats_FailFast)
{
#ifdef _WIN32
    _putenv_s("GE_HEADLESS_TEST", "1");
#else
    setenv("GE_HEADLESS_TEST", "1", 1);
#endif

    DeviceDesc dd{}; dd.preferredAPI = GraphicsAPI::Vulkan; dd.enableDynamicRendering = true; dd.enableDebugLayer = false; dd.enableDescriptorValidation = false;
    auto dev = DeviceFactory::CreateDevice(dd);
    ASSERT_TRUE(dev);
    if (!dev->Initialize(dd)) { GTEST_SKIP() << "Device init failed"; }

    // Build a graphics pipeline with dynamic rendering but without explicit formats
    PipelineDesc pd{}; pd.type = PipelineType::Graphics; pd.debugName = "DynDR_MissingFormats";
    pd.vertexShader = GameEngine::Rendering::Tests::ReadSpirvBytes("fullscreen_noinput.vert.spv");
    pd.pixelShader  = GameEngine::Rendering::Tests::ReadSpirvBytes("copy.frag.spv");
    ASSERT_FALSE(pd.vertexShader.empty()); ASSERT_FALSE(pd.pixelShader.empty());
    pd.EnableDepthTest(false);
    pd.AddDynamicState(DynamicState::Viewport);
    pd.AddDynamicState(DynamicState::Scissor);
    // Intentionally leave pd.colorAttachmentFormats empty and depthAttachmentFormat = 0

    PipelineHandle pipe = dev->CreatePipeline(pd);
    EXPECT_FALSE(pipe.IsValid()) << "Pipeline should fail to create when dynamic rendering is enabled and no formats are provided";
}
