#include "Rendering/Core/Device.h"
#include "Rendering/Core/Pipeline/PipelineDescTranslator.h"
#include "Rendering/Core/RenderGraph/RGFullscreen.h"
#include "Tests/TestUtils.h"
#include "TestTempDir.h"
#include "Platform/Environment.h"
#include "Rendering/Passes/TonemapPass.h"
#include "Rendering/Passes/SRGBEncodePass.h"
#include "Rendering/Passes/PixelPerfectUpscalePass.h"

#include <chrono>
#include <string>

#include <gtest/gtest.h>

using namespace GameEngine::Rendering;

namespace
{
constexpr const char* kPipelineName = "Test.Copy";
constexpr const char* kLayoutName = "Test.Copy.Set0";

double MeasureExecuteMilliseconds(RenderGraph::RGFrame& frame)
{
    const auto begin = std::chrono::steady_clock::now();
    frame.Execute();
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
}
} // namespace

// AddCopyPass binds one combined-image-sampler at set 0 binding 0; the loaded
// desc must carry exactly that layout on canonical fullscreen state.
TEST(RGCopyPipeline, LoadsStockCopyOnFullscreenState)
{
    DescriptorSetLayoutDesc layout{};
    PipelineDesc pipeline{};
    if (!RenderGraph::LoadCopyPipelineDesc(ShaderSourceKind::SpirV, kPipelineName, kLayoutName,
                                           layout, pipeline))
        GTEST_SKIP() << "copy.shaderpkg unavailable in this environment";

    ASSERT_EQ(layout.bindings.size(), 1u);
    EXPECT_EQ(layout.bindings[0].binding, 0u);
    EXPECT_EQ(layout.bindings[0].type, DescriptorType::CombinedImageSampler);
    EXPECT_EQ(layout.bindings[0].shaderStages, kShaderStageFragment);
    EXPECT_STREQ(layout.debugName, kLayoutName);

    EXPECT_FALSE(pipeline.vertexShader.empty());
    EXPECT_FALSE(pipeline.pixelShader.empty());
    EXPECT_EQ(pipeline.type, PipelineType::Graphics);
    EXPECT_EQ(pipeline.rasterizationSamples, 1u);
    EXPECT_STREQ(pipeline.debugName, kPipelineName);
    ASSERT_EQ(pipeline.descriptorSetLayouts.size(), 1u);
    EXPECT_EQ(pipeline.descriptorSetLayouts[0].bindings.size(), 1u);
}

namespace
{
class PreparedCopyTest : public ::testing::TestWithParam<GraphicsAPI>
{
  protected:
    void SetUp() override
    {
        m_PreviousResolver = Utils::GetShaderPathResolver();
        Utils::SetShaderPathResolver(nullptr);
    }
    void TearDown() override { Utils::SetShaderPathResolver(m_PreviousResolver); }

  private:
    Utils::ShaderPathResolverFunc m_PreviousResolver = nullptr;
};
}

TEST_P(PreparedCopyTest, ColdPipelineIsReadyBeforeRecordingAndCopiesTheFirstFrame)
{
    using namespace GameEngine::Rendering::RenderGraph;
    GameEngine::TestUtils::ScopedTempDir pipelineCache(
        GameEngine::TestUtils::MakeUniqueTempDirectory("prepared-copy"));
    DeviceDesc deviceDesc{};
    deviceDesc.pipelineCacheDirectory = pipelineCache.Path().string();
    deviceDesc.preferredAPI = GetParam();
    deviceDesc.enableDynamicRendering = true;
    deviceDesc.enableSwapchain = false;
    auto device = DeviceFactory::CreateDevice(deviceDesc);
    if (!device || !device->Initialize(deviceDesc))
        GTEST_SKIP() << "requested headless backend unavailable: " << static_cast<int>(GetParam());
    ASSERT_EQ(device->GetAPI(), GetParam());

    DescriptorSetLayoutDesc layout;
    PipelineDesc pipelineDesc;
    ASSERT_TRUE(LoadCopyPipelineDesc(device->PreferredShaderSource(), "PreparedCopy", "PreparedCopy.Set0",
                                    layout, pipelineDesc));
    const GraphicsPipelineId pipeline = PipelineDescTranslator::InternGraphics(*device, pipelineDesc);
    ASSERT_TRUE(pipeline.IsValid());
    PipelineFormatKey formatKey{};
    formatKey.ColorCount = 1;
    formatKey.ColorFormats[0] = TextureFormat::RGBA8_UNORM;
    formatKey.RasterizationSamples = 1;
    ASSERT_FALSE(device->TryGetWarmGraphicsPipeline(pipeline, formatKey).IsValid());
    const SamplerHandle sampler = device->CreateSampler(SamplerDesc::PointClamp());
    ASSERT_TRUE(sampler.IsValid());
    constexpr uint32_t kWidth = 4;
    constexpr uint32_t kHeight = 4;
    constexpr uint32_t kBytesPerPixel = 4;
    constexpr uint32_t kReadbackSize = kWidth * kHeight * kBytesPerPixel;
    const BufferHandle readback = device->CreateReadbackBuffer(kReadbackSize);
    ASSERT_TRUE(readback.IsValid());
    RGResourcePool persistent(device.get());
    RGTransientPool transient(device.get());
    RGUploadRing uploads(device.get(), 2, 4096);
    RGFrame frame(device.get(), &persistent, &transient, &uploads);
    frame.BeginFrame(0);
    TextureDesc textureDesc{};
    textureDesc.width = kWidth;
    textureDesc.height = kHeight;
    textureDesc.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
    textureDesc.usage = static_cast<uint32_t>(TextureUsage::RenderTarget | TextureUsage::ShaderResource);
    const RGTexture source = frame.CreateTexture("PreparedCopy.Source", textureDesc);
    const RGTexture output = frame.CreateTexture("PreparedCopy.Output", textureDesc);
    const RGBuffer destination = frame.ImportExternalBuffer("PreparedCopy.Readback", readback, kReadbackSize);
    bool pipelineWasReadyAtFirstRecord = false;
    bool allowRecording = false;
    uint32_t recordCount = 0;
    uint32_t forkCount = 0;
    frame.SetRecordParallel([&forkCount](uint32_t count, const std::function<void(uint32_t)>& record) {
        forkCount += count;
        for (uint32_t index = 0; index < count; ++index)
            record(index);
    });
    frame.AddPass("Source", 0,
                  [source](RGPassBuilder& pass)
                  {
                      pass.AttachColor(0, source, {.Load = RGLoadOp::Clear, .Clear = {.Color = {0, 1, 0, 1}}});
                      pass.RecordInSecondary();
                  },
                  [&recordCount, &pipelineWasReadyAtFirstRecord, &device, pipeline, formatKey](RGContext&)
                  {
                      ++recordCount;
                      pipelineWasReadyAtFirstRecord = device->TryGetWarmGraphicsPipeline(pipeline, formatKey).IsValid();
                  });
    AddCopyPass(frame, source, output, "PreparedCopy", 0, pipeline, {}, layout, {}, sampler);
    frame.AddPass("Readback", 0,
                  [output, destination, &allowRecording, &frame](RGPassBuilder& pass)
                  {
                      pass.Read(output, RGTextureRead::CopySrc);
                      pass.Write(destination, RGBufferWrite::CopyDst);
                      pass.Prepare([&allowRecording, &frame](RGPrepareContext&) {
                          EXPECT_DOUBLE_EQ(frame.Stats().PrepareMs, 0.0);
                          EXPECT_DOUBLE_EQ(frame.Stats().RecordAndSubmitMs, 0.0);
                          return allowRecording;
                      });
                  },
                  [output, readback](RGContext& context)
                  { context.Cmd->CopyTextureSubresourceToBuffer(context.GetTexture(output), 0, 0, readback,
                                                                kWidth, kHeight, 0, 0, 0, 0); });
    frame.MarkOutput(destination);
    const double abortedExecuteMs = MeasureExecuteMilliseconds(frame);
    EXPECT_DOUBLE_EQ(frame.Stats().RecordAndSubmitMs, 0.0);
    EXPECT_LE(frame.Stats().PrepareMs, abortedExecuteMs);
    EXPECT_EQ(frame.Stats().PreparationFailures, 1u);
    EXPECT_EQ(frame.Stats().SubmissionsMade, 0u);
    EXPECT_EQ(frame.SubmittedFrameCount(), 0u);
    EXPECT_EQ(recordCount, 0u);

    allowRecording = true;
    const double successfulExecuteMs = MeasureExecuteMilliseconds(frame);
    EXPECT_GT(frame.Stats().RecordAndSubmitMs, 0.0);
    EXPECT_LE(frame.Stats().PrepareMs + frame.Stats().RecordAndSubmitMs, successfulExecuteMs);
    RecordProperty("prepareMs", std::to_string(frame.Stats().PrepareMs));
    RecordProperty("recordAndSubmitMs", std::to_string(frame.Stats().RecordAndSubmitMs));
    RecordProperty("executeEnvelopeMs", std::to_string(successfulExecuteMs));
    device->WaitForIdle();
    EXPECT_TRUE(pipelineWasReadyAtFirstRecord);
    EXPECT_EQ(recordCount, 1u) << "secondary fallback must not execute a pass twice";
    const bool parallel = GameEngine::Platform::EnvironmentSwitchEnabled("GE_PARALLEL_RECORD", false);
    EXPECT_EQ(forkCount, parallel ? 1u : 0u);
    EXPECT_EQ(frame.Stats().SecondaryPassesEligible, parallel ? 1u : 0u);
    const bool secondary = parallel && device->GetAPI() == GraphicsAPI::Vulkan;
    EXPECT_EQ(frame.Stats().SecondaryPassesRecorded, secondary ? 1u : 0u);
    EXPECT_EQ(frame.Stats().SecondaryPassesFallback, parallel && !secondary ? 1u : 0u);
    EXPECT_EQ(frame.Stats().InlinePassesRecorded, secondary ? 2u : 3u);
    if (!secondary)
        EXPECT_DOUBLE_EQ(frame.Stats().WorkerRecordMs, 0.0);
    EXPECT_EQ(frame.Stats().PreparedPasses, 4u);
    EXPECT_EQ(frame.Stats().PreparationFailures, 1u);
    EXPECT_EQ(frame.SubmittedFrameCount(), 1u);

    allowRecording = false;
    const double rejectedRetryMs = MeasureExecuteMilliseconds(frame);
    EXPECT_DOUBLE_EQ(frame.Stats().RecordAndSubmitMs, 0.0);
    EXPECT_LE(frame.Stats().PrepareMs, rejectedRetryMs);
    EXPECT_EQ(frame.Stats().PreparationFailures, 2u);
    EXPECT_EQ(frame.SubmittedFrameCount(), 1u);
    EXPECT_EQ(recordCount, 1u);
    const auto* pixels = static_cast<const uint8_t*>(device->MapBuffer(readback));
    ASSERT_NE(pixels, nullptr);
    for (uint32_t offset = 0; offset < kReadbackSize; offset += kBytesPerPixel)
    {
        EXPECT_LE(pixels[offset], 1u);
        EXPECT_GE(pixels[offset + 1], 254u);
        EXPECT_LE(pixels[offset + 2], 1u);
        EXPECT_GE(pixels[offset + 3], 254u);
    }
    device->UnmapBuffer(readback);
    device->DestroyBuffer(readback);
    device->DestroySampler(sampler);
}

TEST_P(PreparedCopyTest, FinalizeChainPreparesDescriptorsAndDrawsTheColdFirstFrame)
{
    using namespace GameEngine::Rendering::RenderGraph;
    GameEngine::TestUtils::ScopedTempDir cache(
        GameEngine::TestUtils::MakeUniqueTempDirectory("prepared-finalize"));
    DeviceDesc deviceDesc{};
    deviceDesc.pipelineCacheDirectory = cache.Path().string();
    deviceDesc.preferredAPI = GetParam();
    deviceDesc.enableDynamicRendering = true;
    deviceDesc.enableSwapchain = false;
    auto device = DeviceFactory::CreateDevice(deviceDesc);
    if (!device || !device->Initialize(deviceDesc))
        GTEST_SKIP() << "requested headless backend unavailable";
    ASSERT_EQ(device->GetAPI(), GetParam());
    constexpr uint32_t kExtent = 4;
    constexpr uint32_t kPixelBytes = 4;
    constexpr uint32_t kReadbackBytes = kExtent * kExtent * kPixelBytes;
    const BufferHandle readback = device->CreateReadbackBuffer(kReadbackBytes);
    ASSERT_TRUE(readback.IsValid());
    RGResourcePool persistent(device.get());
    RGTransientPool transient(device.get());
    RGUploadRing uploads(device.get(), 2, 4096);
    RGFrame frame(device.get(), &persistent, &transient, &uploads);
    frame.BeginFrame(0);
    TextureDesc textureDesc{};
    textureDesc.width = textureDesc.height = kExtent;
    textureDesc.format = static_cast<uint32_t>(TextureFormat::R16G16B16A16_FLOAT);
    textureDesc.usage = static_cast<uint32_t>(TextureUsage::RenderTarget | TextureUsage::ShaderResource);
    const auto source = frame.CreateTexture("Finalize.Source", textureDesc);
    const auto linear = frame.CreateTexture("Finalize.Linear", textureDesc);
    textureDesc.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
    const auto encoded = frame.CreateTexture("Finalize.Encoded", textureDesc);
    const auto output = frame.CreateTexture("Finalize.Output", textureDesc);
    const auto destination = frame.ImportExternalBuffer("Finalize.Readback", readback, kReadbackBytes);
    const uint32_t allocatedBefore = device->GetDescriptorAllocatorStats().TransientAllocated;
    bool observedPreparation = false;
    frame.AddPass("Finalize.Source", 0,
                  [source](RGPassBuilder& pass) {
                      pass.AttachColor(0, source, {.Load = RGLoadOp::Clear, .Clear = {.Color = {0, 1, 0, 1}}});
                  },
                  [&observedPreparation, &frame, &device, allocatedBefore](RGContext&) {
                      observedPreparation = frame.Stats().PreparedPasses == 3u;
                      if (device->GetAPI() == GraphicsAPI::Vulkan &&
                          GameEngine::Platform::EnvironmentSwitchEnabled("GE_VK_CAPTURE_COMPAT", false))
                          EXPECT_GE(device->GetDescriptorAllocatorStats().TransientAllocated, allocatedBefore + 3u)
                              << "all finalize descriptors must exist before the first record callback";
                  });
    Passes::TonemapParams tonemap{};
    tonemap.tonemapMode = 5; // Linear keeps saturated green unchanged.
    ASSERT_TRUE(Passes::AddTonemapPassRG(frame, source, linear, tonemap).IsValid());
    ASSERT_TRUE(Passes::AddSRGBEncodePassRG(
        frame, linear, encoded, {Passes::FinalizeInputSpace::Linear, Passes::FinalizeQuantizer::None}).IsValid());
    Passes::PixelPerfectUpscaleParamsRG upscale{};
    upscale.Src = encoded;
    upscale.Dst = output;
    upscale.ReferenceWidth = upscale.ReferenceHeight = 2;
    upscale.SourceWidth = upscale.SourceHeight = kExtent;
    upscale.Zoom = 2;
    ASSERT_TRUE(Passes::AddPixelPerfectUpscalePassRG(
        frame, upscale, Passes::FinalizeInputSpace::EncodedSrgb).IsValid());
    frame.AddPass("Finalize.Readback", 0,
                  [output, destination](RGPassBuilder& pass) {
                      pass.Read(output, RGTextureRead::CopySrc);
                      pass.Write(destination, RGBufferWrite::CopyDst);
                  },
                  [output, readback](RGContext& context) {
                      context.Cmd->CopyTextureSubresourceToBuffer(context.GetTexture(output), 0, 0, readback,
                                                                 kExtent, kExtent, 0, 0, 0, 0);
                  });
    frame.MarkOutput(destination);
    frame.Execute();
    device->WaitForIdle();
    EXPECT_TRUE(observedPreparation);
    EXPECT_EQ(frame.Stats().PreparationFailures, 0u);
    const auto* pixels = static_cast<const uint8_t*>(device->MapBuffer(readback));
    ASSERT_NE(pixels, nullptr);
    for (uint32_t offset = 0; offset < kReadbackBytes; offset += kPixelBytes)
    {
        EXPECT_LE(pixels[offset], 1u);
        EXPECT_GE(pixels[offset + 1], 254u);
        EXPECT_LE(pixels[offset + 2], 1u);
        EXPECT_GE(pixels[offset + 3], 254u);
    }
    device->UnmapBuffer(readback);
    device->DestroyBuffer(readback);
    Passes::CleanupTonemapPassForDevice(device.get());
    Passes::CleanupSRGBEncodePassForDevice(device.get());
    Passes::CleanupPixelPerfectUpscalePassForDevice(device.get());
}

TEST_P(PreparedCopyTest, OnlyLiveRequiredPipelinesBuildForTheirActualAttachments)
{
    using namespace GameEngine::Rendering::RenderGraph;
    GameEngine::TestUtils::ScopedTempDir pipelineCache(
        GameEngine::TestUtils::MakeUniqueTempDirectory("required-pipeline"));
    DeviceDesc deviceDesc{};
    deviceDesc.pipelineCacheDirectory = pipelineCache.Path().string();
    deviceDesc.preferredAPI = GetParam();
    deviceDesc.enableDynamicRendering = true;
    deviceDesc.enableSwapchain = false;
    auto device = DeviceFactory::CreateDevice(deviceDesc);
    if (!device || !device->Initialize(deviceDesc))
        GTEST_SKIP() << "requested headless backend unavailable";
    ASSERT_EQ(device->GetAPI(), GetParam());
    DescriptorSetLayoutDesc layout;
    PipelineDesc pipelineDesc;
    ASSERT_TRUE(LoadCopyPipelineDesc(device->PreferredShaderSource(), "RequiredCopy", "RequiredCopy.Set0",
                                    layout, pipelineDesc));
    const GraphicsPipelineId pipeline = PipelineDescTranslator::InternGraphics(*device, pipelineDesc);
    ASSERT_TRUE(pipeline.IsValid());
    PipelineFormatKey liveKey{};
    liveKey.ColorCount = 1;
    liveKey.ColorFormats[0] = TextureFormat::R16G16B16A16_FLOAT;
    liveKey.RasterizationSamples = 4;
    PipelineFormatKey deadKey = liveKey;
    deadKey.ColorFormats[0] = TextureFormat::RGBA8_UNORM;
    deadKey.RasterizationSamples = 1;
    RGResourcePool persistent(device.get());
    RGTransientPool transient(device.get());
    RGUploadRing uploads(device.get(), 2, 4096);
    RGFrame frame(device.get(), &persistent, &transient, &uploads);
    frame.BeginFrame(0);
    TextureDesc textureDesc{};
    textureDesc.width = 4;
    textureDesc.height = 4;
    textureDesc.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
    textureDesc.usage = static_cast<uint32_t>(TextureUsage::RenderTarget | TextureUsage::ShaderResource);
    const RGTexture dead = frame.CreateTexture("Required.Dead", textureDesc);
    textureDesc.format = static_cast<uint32_t>(TextureFormat::R16G16B16A16_FLOAT);
    const RGTexture resolve = frame.CreateTexture("Required.Resolve", textureDesc);
    textureDesc.sampleCount = 4;
    const RGTexture multisample = frame.CreateTexture("Required.Msaa", textureDesc);
    frame.AddPass("Dead", 0,
                  [dead, pipeline](RGPassBuilder& pass) { pass.AttachColor(0, dead); pass.UsePipeline(pipeline); },
                  [](RGContext&) { ADD_FAILURE() << "culled pass recorded"; });
    bool readyBeforeRecord = false;
    frame.AddPass("Required", 0,
                  [multisample, resolve, pipeline](RGPassBuilder& pass)
                  {
                      pass.AttachColorResolve(0, multisample, resolve, {.Load = RGLoadOp::Clear});
                      pass.UsePipeline(pipeline);
                  },
                  [&readyBeforeRecord, &device, pipeline, liveKey](RGContext& context)
                  {
                      EXPECT_EQ(context.BuildCurrentFormatKey(), liveKey);
                      readyBeforeRecord = device->TryGetWarmGraphicsPipeline(pipeline, liveKey).IsValid();
                  });
    frame.MarkOutput(resolve);
    frame.Execute();
    EXPECT_TRUE(readyBeforeRecord);
    EXPECT_EQ(frame.Stats().PreparedPasses, 1u);
    EXPECT_FALSE(device->TryGetWarmGraphicsPipeline(pipeline, deadKey).IsValid());
}

namespace
{
constexpr GraphicsAPI kPreparedCopyBackends[]{
    GraphicsAPI::Vulkan,
#if defined(RENDERING_HAS_METAL)
    GraphicsAPI::Metal,
#endif
};
}
INSTANTIATE_TEST_SUITE_P(RenderBackends, PreparedCopyTest, ::testing::ValuesIn(kPreparedCopyBackends));
