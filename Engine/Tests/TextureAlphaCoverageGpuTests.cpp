#include "Assets/AssetManager.h"
#include "Assets/TextureCook.h"
#include "Core/Application.h"
#include "Core/Engine.h"
#include "ECS/World.h"
#include "ECSModules/Rendering/Systems/MaterialMipDemandSystem.h"
#include "EngineLogCapture.h"
#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/MaterialMipStreaming/MaterialMipBudget.h"
#include "Engine/Rendering/MaterialRegistry.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/TextureService.h"
#include "Mathematics/MatrixOps.h"
#include "Memory/StatsSnapshot.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/GPUScene.h"
#include "StagedTestPaths.h"
#include "TestCameraEye.h"
#include "TestDeviceHelper.h"
#include "TextureCookTestFixtures.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

using namespace GameEngine;
using namespace GameEngine::Rendering;

namespace GameEngine::Engine::Renderer
{
// Seed the existing worker-completion boundary without depending on thread
// timing. All admission, GPU publication and pending-bind cleanup run normally.
struct TextureCoverageUploadTestAccess
{
    static void Queue(TextureService& service, const GUID& texture, const GUID& material,
                      const SharedPtr<Asset>& decoded, StringId slot = StringId{})
    {
        std::lock_guard lock(service.m_PendingTexturesMutex);
        const auto epoch = service.m_TextureEpochByGuid[texture];
        service.m_PendingGpuUploads.push_back({texture, decoded, epoch});
        service.m_PendingTextureBinds.push_back({material, slot, texture});
        service.m_TextureLoadsKicked.insert(texture);
        service.m_TextureLoadHandles.try_emplace(texture);
    }
    static bool HasLoadHandle(TextureService& service, const GUID& texture)
    {
        std::lock_guard lock(service.m_PendingTexturesMutex);
        return service.m_TextureLoadHandles.contains(texture);
    }
    static size_t Queued(TextureService& service, const GUID& texture)
    {
        std::lock_guard lock(service.m_PendingTexturesMutex);
        return std::count_if(service.m_PendingGpuUploads.begin(), service.m_PendingGpuUploads.end(),
            [&](const auto& pending) { return pending.TextureGuid == texture; });
    }
    static bool InFlight(TextureService& service, const GUID& texture)
    {
        std::lock_guard lock(service.m_PendingTexturesMutex);
        return service.m_TextureLoadsKicked.contains(texture);
    }
    static void HoldAdmission(TextureService& service, const GUID& texture)
    {
        std::lock_guard lock(service.m_PendingTexturesMutex);
        service.m_TextureLoadsKicked.insert(texture);
    }
    static bool UploadJobsIdle(TextureService& service) { return service.m_UploadJobCounter.IsZero(); }
    // What an Evict does to a load's epoch, without the rest of the Evict: a queued result stays.
    static void SupersedeLoadEpoch(TextureService& service, const GUID& texture)
    {
        std::lock_guard lock(service.m_PendingTexturesMutex);
        ++service.m_TextureEpochByGuid[texture];
    }
    static uint64 MipBudget(const TextureService& service) { return service.m_MaterialMipBudgetBytes; }
    static void SetMipBudget(TextureService& service, uint64 bytes)
    {
        service.m_MaterialMipBudgetBytes = bytes;
        service.m_MaterialMipStreamsSettled = false;
    }
    static std::vector<float> MipCoverage(TextureService& service)
    {
        std::lock_guard lock(service.m_PendingTexturesMutex);
        return service.m_MaterialCoverage;
    }
    static bool MipStreamsSettled(TextureService& service)
    {
        std::lock_guard lock(service.m_PendingTexturesMutex);
        return service.m_MaterialMipStreamsSettled;
    }
    static TextureHandle StreamedHandle(TextureService& service, const GUID& guid)
    {
        std::lock_guard lock(service.m_PendingTexturesMutex);
        const auto* stream = service.m_MaterialMipStreams.Find(guid);
        return !stream ? TextureHandle{} : stream->Resident.Texture;
    }
    static void HotSwap(TextureService& service, const GUID& texture) { service.HotSwapTexture(texture); }
    static TextureHandle StreamedCompanion(TextureService& service, const GUID& guid)
    {
        std::lock_guard lock(service.m_PendingTexturesMutex);
        const auto* stream = service.m_MaterialMipStreams.Find(guid);
        return !stream ? TextureHandle{} : stream->Resident.NormalVarianceCompanion;
    }
    static bool Uploaded(TextureService& service, const GUID& texture)
    {
        return service.m_TextureGPUCache.Contains(texture);
    }
};
} // namespace GameEngine::Engine::Renderer

namespace
{
namespace fs = std::filesystem;
using MipStreamState = Engine::Renderer::TextureService::MaterialMipStreamState;

// The stream of `texture` in `report`, or null.
const MipStreamState* FindMipStream(const Engine::Renderer::TextureService::MaterialMipStreamReport& report,
                                    const GUID& texture)
{
    const auto found = std::ranges::find(report.Streams, texture, &MipStreamState::Texture);
    return found == report.Streams.end() ? nullptr : &*found;
}

// The report of `textures` names the first resident level and the coverage that the last plan used.
void ExpectMipStreamReport(const Engine::Renderer::TextureService& service, std::span<const GUID> textures,
                           std::span<const uint32> firstMips, std::span<const float> coverage)
{
    const auto report = service.ReportMaterialMipStreams();
    EXPECT_EQ(report.BudgetBytes, 2u * 1024u * 1024u);
    ASSERT_EQ(report.Streams.size(), textures.size());
    for (size_t index = 0; index < textures.size(); ++index)
    {
        const MipStreamState* stream = FindMipStream(report, textures[index]);
        ASSERT_NE(stream, nullptr);
        EXPECT_EQ(stream->FirstMip, firstMips[index]);
        EXPECT_EQ(stream->Tail, 1u);
        EXPECT_EQ(stream->TopExtent, 512u);
        EXPECT_FLOAT_EQ(stream->ScreenCoverage, coverage[index]);
        EXPECT_FALSE(stream->StepTargetMip.has_value());
    }
}
class TextureCoverageGPU : public testing::Test
{
  protected:
    inline static fs::path s_Workspace, s_PreviousDirectory;
    std::unique_ptr<IDevice> m_Device;
    Engine::Renderer::MaterialRegistry m_Materials;
    Engine::Renderer::TextureService m_Textures;
    PipelineHandle m_Pipeline;
    SamplerHandle m_Sampler;
    DescriptorSetLayoutDesc m_Layout;

    static void SetUpTestSuite()
    {
        s_PreviousDirectory = fs::current_path();
        s_Workspace = fs::temp_directory_path() / ("ge-coverage-gpu-" + GUID::Generate().ToString());
        fs::create_directories(s_Workspace / "Assets");
        // EngineCore canonicalizes its workspace. Match it on hosts where /var
        // is a symlink to /private/var, or metadata writes target an apparent
        // out-of-project asset and never exercise the coverage policy at all.
        s_Workspace = fs::canonical(s_Workspace);
        auto& engine = EngineCore::GetInstance();
        ScriptsConfig scripts;
        scripts.disableClr = true;
        scripts.enableHotReload = false;
        scripts.enableAsyncHotReload = false;
        scripts.enableAutoProjectGeneration = false;
        engine.SetScriptsConfig(scripts);
        ApplicationConfig config;
        config.WorkspaceDirectory = s_Workspace.string();
        config.EnableEditor = false;
        ASSERT_TRUE(engine.Initialize(config));
    }
    static void TearDownTestSuite()
    {
        EngineCore::GetInstance().Shutdown();
        fs::current_path(s_PreviousDirectory);
        std::error_code error;
        fs::remove_all(s_Workspace, error);
    }
    void SetUp() override
    {
        m_Device = CreateVulkanDeviceFast();
        if (!m_Device)
            GTEST_SKIP() << "Vulkan m_Device unavailable";
        m_Materials.Initialize(m_Device.get());
        ASSERT_TRUE(m_Textures.Initialize(m_Device.get(), m_Materials,
                                          RendererProfile::FromCapabilities(m_Device->GetCapabilities())));
        m_Sampler = m_Device->CreateSampler(SamplerDesc::MaterialLinearRepeat("Coverage.Trilinear"));
        ASSERT_TRUE(m_Sampler.IsValid());
        DescriptorBinding binding{};
        binding.binding = 0;
        binding.type = DescriptorType::CombinedImageSampler;
        binding.count = 1;
        binding.shaderStages = kShaderStageFragment;
        m_Layout.bindings.push_back(binding);
        PipelineDesc desc{};
        desc.type = PipelineType::Graphics;
        desc.vertexShader = Read(TestPaths::StagedRoot() / "Shaders/texture_coverage_probe.vert.spv");
        desc.pixelShader = Read(TestPaths::StagedRoot() / "Shaders/texture_coverage_probe.frag.spv");
        ASSERT_FALSE(desc.vertexShader.empty());
        ASSERT_FALSE(desc.pixelShader.empty());
        desc.topology = PrimitiveTopology::TriangleList;
        desc.AddDynamicState(DynamicState::Viewport);
        desc.AddDynamicState(DynamicState::Scissor);
        desc.EnableDepthTest(false);
        desc.SetCullingMode(CullModeFlagBits::None);
        desc.descriptorSetLayouts = {m_Layout};
        desc.pushConstantSize = 24;
        desc.pushConstantStagesMask = kShaderStageFragment;
        desc.colorAttachmentFormats = {uint32_t(Rendering::TextureFormat::R32G32B32A32_FLOAT)};
        m_Pipeline = m_Device->CreatePipeline(desc);
        ASSERT_TRUE(m_Pipeline.IsValid());
    }
    void TearDown() override
    {
        if (!m_Device)
            return;
        m_Device->WaitForIdle();
        if (m_Pipeline.IsValid())
            m_Device->DestroyPipeline(m_Pipeline);
        if (m_Sampler.IsValid())
            m_Device->DestroySampler(m_Sampler);
        m_Textures.Shutdown();
        m_Materials.Shutdown();
        m_Device.reset();
    }
    static std::vector<uint8> Read(const fs::path& path)
    {
        std::ifstream stream(path, std::ios::binary);
        return std::vector<uint8>(std::istreambuf_iterator<char>(stream), {});
    }
    // A square TGA of `size` texels a side (64 unless a test needs a slow decode).
    fs::path Image(bool grayAlpha = false, int size = 64)
    {
        const auto path = s_Workspace / "Assets" / (GUID::Generate().ToString() + ".tga");
        std::vector<uint8> bytes(18, 0);
        bytes[2] = grayAlpha ? 3 : 2;
        bytes[12] = uint8(size & 0xff);
        bytes[13] = uint8(size >> 8);
        bytes[14] = uint8(size & 0xff);
        bytes[15] = uint8(size >> 8);
        bytes[16] = grayAlpha ? 16 : 32;
        bytes[17] = 0x28;
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x)
            {
                // Repeated small, irregular leaf islands produce meaningful erosion
                // at intermediate levels while retaining tied coarse tail levels.
                const int dx = x % 16 - 7, dy = y % 16 - 7;
                const uint8 alpha = dx * dx + 2 * dy * dy < 37 ? 255 : 0;
                if (grayAlpha)
                    bytes.insert(bytes.end(), {uint8(91 + x + y), alpha});
                else
                    bytes.insert(bytes.end(), {uint8(51 + x), uint8(77 + y), uint8(91 + x + y), alpha});
            }
        std::ofstream stream(path, std::ios::binary);
        stream.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        stream.close();
        return path;
    }
    void Set(const fs::path& path, const char* key, const char* value)
    {
        ASSERT_TRUE(EngineCore::GetInstance().GetAssetManager().GetRegistry().SetMetaValue(path, key, value));
    }
    GUID Register(const fs::path& path, bool coverage)
    {
        Set(path, kTextureCompressionMetaKey, "none");
        Set(path, kTextureColorSpaceMetaKey, "srgb");
        Set(path, kTextureAlphaCoverageMetaKey, coverage ? "1" : "0");
        Set(path, kTextureAlphaCutoffMetaKey, ".538");
        return EngineCore::GetInstance().GetAssetManager().GetRegistry().GetAssetGUID(path);
    }
    // A cooked RGBA8 texture of `size` texels a side (a power of 2) in a file, and its loaded asset.
    void MakeStreamedTexture(GUID& texture, SharedPtr<TextureAsset>& asset, int size = 512)
    {
        const auto source = Read(Image(false, size));
        TextureCookInputs inputs;
        std::vector<uint8> cooked;
        std::string error;
        ASSERT_TRUE(CookTexture(source.data(), source.size(), "streamed.tga", ".tga", inputs, TextureCookOutput::Uncompressed,
                                cooked, error)) << error;
        const auto path = s_Workspace / "Assets" / (GUID::Generate().ToString() + ".ktx2");
        {
            std::ofstream stream(path, std::ios::binary);
            stream.write(reinterpret_cast<const char*>(cooked.data()), cooked.size());
            ASSERT_TRUE(stream.good());
        }
        texture = Register(path, false);
        ASSERT_FALSE(texture.IsNull());
        asset = MakeShared<TextureAsset>(texture, path);
        ASSERT_TRUE(asset->LoadFromData(cooked));
        ASSERT_EQ(asset->GetMipChain().size(), static_cast<size_t>(std::bit_width(static_cast<uint32>(size))));
    }

    // Runs frames until the streams have no step left. A step reads its levels on an I/O thread.
    void SettleMipStreams()
    {
        constexpr int kMaximumFrames = 400;
        for (int frame = 0; frame < kMaximumFrames; ++frame)
        {
            m_Textures.FlushPendingUploads();
            if (Engine::Renderer::TextureCoverageUploadTestAccess::MipStreamsSettled(m_Textures))
                return;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        ADD_FAILURE() << "the material mip streams did not settle";
    }

    static ViewId AddMipDemandView(Engine::Renderer::ViewRegistry& views, const char* name, const ECS::World& world)
    {
        constexpr float kNear = 0.1f;
        constexpr float kFar = 100.0f;
        CameraData camera{};
        camera.view = Mathematics::Matrix4x4::Identity();
        camera.proj = Mathematics::MakeOrthographicLH_ZO_ReverseZ(-8.0f, 8.0f, -4.0f, 4.0f, kNear, kFar);
        camera.viewProj = camera.proj;

        const CameraId cameraId = views.AllocateCamera(name);
        views.SetCameraData(cameraId, GameEngine::Tests::TestCameraWithEye(camera), {kNear, kFar});
        const ViewId view = views.AllocateView(name, cameraId);
        views.SetViewWorldId(view, world.GetWorldId());
        return view;
    }

    Engine::Renderer::Material* RegisterStreamingMaterial(const GUID& materialGuid, const char* name)
    {
        MaterialDocument doc{};
        doc.schemaVersion = 3;
        doc.materialName = name;
        doc.lightingModel = "StandardPBR";
        doc.surfaceShader = "Surfaces/standard_pbr.glsl";
        return m_Materials.Register(materialGuid, doc);
    }

    // Pumps the asset manager and the upload drain until the slot of `material` has a texture.
    TextureHandle WaitForBoundTexture(const Engine::Renderer::Material& material, StringId slot)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (!material.GetTexture(slot).IsValid() && std::chrono::steady_clock::now() < deadline)
        {
            EngineCore::GetInstance().GetAssetManager().Update();
            m_Textures.FlushPendingUploads();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return material.GetTexture(slot);
    }

    // Puts bytes that are no texture in the file of `asset`, as a writer that replaces the file does.
    void ReplaceFileOf(const TextureAsset& asset)
    {
        std::ofstream stream(asset.GetPath(), std::ios::binary | std::ios::trunc);
        stream << "invalid texture";
        ASSERT_TRUE(stream.good());
    }

    // The cooked artifacts the derived cache holds for `texture`.
    static size_t CookedArtifacts(const GUID& texture)
    {
        const auto root = EngineCore::GetInstance().GetAssetManager().GetRegistry().TryGetCacheRoot(texture);
        if (!root)
            return 0;
        size_t count = 0;
        std::error_code error;
        for (const auto& entry : fs::directory_iterator(*root / "Tex", error))
            count += entry.path().filename().string().rfind(texture.ToString(), 0) == 0 ? 1 : 0;
        return count;
    }
    // Pumps the asset manager and the upload drain until GetOrUpload's load of the texture has
    // an outcome (an upload, or a refusal cached as no texture), and returns it.
    TextureHandle WaitForUpload(const GUID& texture)
    {
        using Access = Engine::Renderer::TextureCoverageUploadTestAccess;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        TextureHandle handle = m_Textures.GetOrUpload(texture);
        while (!Access::Uploaded(m_Textures, texture) && std::chrono::steady_clock::now() < deadline)
        {
            EngineCore::GetInstance().GetAssetManager().Update();
            m_Textures.FlushPendingUploads();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            handle = m_Textures.GetOrUpload(texture);
        }
        return handle;
    }

    // The bytes of the "Rendering.MaterialMipStreams" record: 0 before a stream reports.
    static uint64 StreamedBytesInMemoryStats()
    {
        for (const Memory::StatsEntry& entry : Memory::SnapshotStats())
            if (entry.Name == "Rendering.MaterialMipStreams")
                return entry.Bytes;
        return 0;
    }

    static uint64 LevelPayloadBytes(const TextureLevelFile& file)
    {
        uint64 bytes = 0;
        for (const TextureLevelFile::Level& level : file.Levels())
            bytes += level.Bytes;
        return bytes;
    }

    uint32 WidthOf(TextureHandle texture)
    {
        uint32 width = 0, height = 0;
        m_Device->GetTextureSize(texture, width, height);
        return width;
    }

    std::vector<float> Render(TextureHandle texture, uint32 width, uint32 height, float lod,
                              uint32 mode = 0, float phase = 0.0f, float seed = 0.0f)
    {
        TextureDesc desc{};
        desc.width = width;
        desc.height = height;
        desc.format = uint32_t(Rendering::TextureFormat::R32G32B32A32_FLOAT);
        desc.usage = uint32_t(TextureUsage::RenderTarget | TextureUsage::TransferSrc);
        const auto target = m_Device->CreateTexture(desc);
        const auto readback = m_Device->CreateReadbackBuffer(size_t(width) * height * 16);
        const auto set = m_Device->CreateDescriptorSet({m_Layout, "Coverage.Image", true});
        EXPECT_TRUE(target.IsValid());
        EXPECT_TRUE(readback.IsValid());
        EXPECT_TRUE(set.IsValid());
        m_Device->UpdateCombinedImageSamplerBinding(set, 0, texture, m_Sampler);
        struct Push
        {
            float lod, cutoff, phaseX, phaseY;
            uint32 mode;
            float seed;
        };
        const Push push{lod, .538f, phase, phase * .37f, mode, seed};
        auto commands = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        commands->Begin();
        commands->Barrier(ResourceBarrier::CreateTextureBarrier(target, ResourceState::Undefined, ResourceState::RenderTarget));
        RenderPassDesc pass{};
        pass.colorTargetCount = 1;
        pass.colorTargets[0] = target;
        pass.clearColor[0] = true;
        pass.colorStoreOp[0] = RenderPassDesc::StoreOp::Store;
        std::fill_n(pass.clearColorValue[0], 4, 0.0f);
        commands->BeginRenderPass(pass);
        commands->SetViewport(0, 0, static_cast<float>(width), static_cast<float>(height));
        commands->SetScissor(0, 0, width, height);
        commands->SetPipeline(m_Pipeline);
        commands->BindDescriptorSet(0, set, m_Pipeline);
        commands->SetPushConstants(push);
        commands->Draw(6);
        commands->EndRenderPass();
        commands->Barrier(ResourceBarrier::CreateTextureBarrier(target, ResourceState::RenderTarget, ResourceState::CopySource));
        commands->CopyTextureToBuffer(target, readback, width, height);
        commands->End();
        m_Device->ExecuteCommandLists({commands.get()});
        m_Device->WaitForIdle();
        const auto* mapped = static_cast<const float*>(m_Device->MapBuffer(readback));
        EXPECT_NE(mapped, nullptr);
        std::vector<float> pixels;
        if (mapped)
            pixels.assign(mapped, mapped + size_t(width) * height * 4);
        m_Device->UnmapBuffer(readback);
        m_Device->DestroyBuffer(readback);
        m_Device->DestroyTexture(target);
        m_Device->DestroyDescriptorSet(set);
        return pixels;
    }
    void CheckBlockCompressedMipStream(const fs::path& source, const char* compression,
                                       GameEngine::TextureFormat format, uint64 tailBudgetBytes,
                                       uint32 fullHeight = 512)
    {
        using Access = Engine::Renderer::TextureCoverageUploadTestAccess;
        if (!IsTextureCookEncoderAvailable())
            GTEST_SKIP() << "BC encoder unavailable";
        if (!m_Device->GetCapabilities().supportsTextureCompressionBC)
            GTEST_SKIP() << "the device samples no BC format";

        constexpr uint32 kFullWidth = 512;
        constexpr uint32 kTailWidth = kFullWidth / 2;
        constexpr uint64 kFullBudgetBytes = 1024 * 1024;
        Access::SetMipBudget(m_Textures, tailBudgetBytes);
        const GUID texture = Register(source, false);
        ASSERT_FALSE(texture.IsNull());
        Set(source, kTextureCompressionMetaKey, compression);
        if (format == GameEngine::TextureFormat::BC4 || format == GameEngine::TextureFormat::BC6H)
            Set(source, kTextureColorSpaceMetaKey, "linear");

        const GUID materialGuid = GUID::Generate();
        auto* material = RegisterStreamingMaterial(materialGuid, "MipStreamingBlockCompressed");
        ASSERT_NE(material, nullptr);
        const StringId slot = HashStringId("albedoMap");
        m_Textures.TrackMaterialTextureRefForTesting(materialGuid, slot, texture);
        m_Textures.ScheduleMaterialTextureLoad(texture, materialGuid, slot);
        const auto tail = WaitForBoundTexture(*material, slot);
        ASSERT_EQ(WidthOf(tail), kTailWidth);
        const auto loaded =
            std::dynamic_pointer_cast<TextureAsset>(EngineCore::GetInstance().GetAssetManager().GetAsset(texture));
        ASSERT_NE(loaded, nullptr);
        ASSERT_EQ(loaded->GetFormat(), format);
        ASSERT_EQ(loaded->GetHeight(), fullHeight);
        EXPECT_EQ(loaded->GetPixelData(), nullptr);

        const auto direct = WaitForUpload(texture);
        ASSERT_TRUE(direct.IsValid());
        EXPECT_EQ(WidthOf(direct), kFullWidth);
        constexpr uint32 kProbeExtent = 16;
        const uint32 tailProbeHeight = std::min(kProbeExtent, fullHeight / 2);
        EXPECT_EQ(Render(tail, kProbeExtent, tailProbeHeight, 0), Render(direct, kProbeExtent, tailProbeHeight, 1));

        Access::SetMipBudget(m_Textures, kFullBudgetBytes);
        SettleMipStreams();
        const auto promoted = material->GetTexture(slot);
        ASSERT_EQ(WidthOf(promoted), kFullWidth);
        const uint32 fullProbeHeight = std::min(kProbeExtent, fullHeight);
        EXPECT_EQ(Render(promoted, kProbeExtent, fullProbeHeight, 0),
                  Render(direct, kProbeExtent, fullProbeHeight, 0));
        constexpr uint32 kProbeLod = 3;
        const uint32 probeWidth = kFullWidth >> kProbeLod;
        const uint32 probeHeight = fullHeight >> kProbeLod;
        const auto promotedPixels = Render(promoted, probeWidth, probeHeight, kProbeLod);
        EXPECT_EQ(promotedPixels, Render(direct, probeWidth, probeHeight, kProbeLod));
        if (format == GameEngine::TextureFormat::BC6H)
        {
            ASSERT_FALSE(promotedPixels.empty());
            EXPECT_GT(*std::max_element(promotedPixels.begin(), promotedPixels.end()), 1.0f);
        }

        m_Textures.OnMaterialUnregistered(materialGuid);
        m_Materials.Unregister(materialGuid);
        m_Textures.FlushPendingUploads();
    }

    static double Coverage(const std::vector<float>& pixels)
    {
        size_t passing = 0;
        for (size_t i = 3; i < pixels.size(); i += 4)
            passing += pixels[i] > .5f;
        return double(passing) / (pixels.size() / 4);
    }
    static float LinearByte(uint8 value)
    {
        const float srgb = value / 255.0f;
        return srgb <= 0.04045f ? srgb / 12.92f : std::pow((srgb + 0.055f) / 1.055f, 2.4f);
    }
};
} // namespace

#if defined(GE_HAVE_KTX)
TEST_F(TextureCoverageGPU, RawUploadAndWorkerCookMatchEveryTexelThroughOneByOne)
{
    for (bool grayAlpha : {false, true})
    {
        SCOPED_TRACE(grayAlpha);
        const auto path = Image(grayAlpha);
        const GUID guid = Register(path, true);
        ASSERT_FALSE(guid.IsNull());
        auto decoded = MakeShared<TextureAsset>(guid, path);
        ASSERT_TRUE(decoded->Load());
        ASSERT_EQ(decoded->GetChannels(), grayAlpha ? 2u : 4u);
        Engine::Renderer::TextureCoverageUploadTestAccess::Queue(m_Textures, guid, GUID::Generate(), decoded);
        m_Textures.FlushPendingUploads();
        const auto raw = m_Textures.GetOrUpload(guid);
        ASSERT_TRUE(raw.IsValid());
        std::vector<std::vector<float>> rawLevels;
        for (uint32 level = 0, extent = 64; level < 7; ++level, extent = std::max(extent / 2, 1u))
            rawLevels.push_back(Render(raw, extent, extent, float(level)));
        auto& assets = EngineCore::GetInstance().GetAssetManager();
        {
            AssetManager::ScopedThreadAssetManager context(&assets);
            TextureAsset cooked(guid, path);
            ASSERT_TRUE(cooked.Load());
            ASSERT_EQ(cooked.GetMipChain().size(), 7u);
            for (uint32 level = 0; level < 7; ++level)
            {
                const auto& mip = cooked.GetMipChain()[level];
                ASSERT_EQ(rawLevels[level].size(), size_t(mip.Width) * mip.Height * 4);
                for (size_t i = 0; i < rawLevels[level].size(); ++i)
                {
                    const auto byte = cooked.GetPixelData()[mip.Offset + i];
                    if (i % 4 == 3)
                        EXPECT_NEAR(rawLevels[level][i], byte / 255.0f, 1e-6f);
                    else
                    {
                        // Hardware sRGB decode is approximate. Bound it by the
                        // neighbouring source codes; raw/adopted GPU arrays below
                        // must still match exactly, including every RGB value.
                        const auto lower = uint8(std::max(int(byte) - 1, 0));
                        const auto upper = uint8(std::min(int(byte) + 1, 255));
                        EXPECT_GE(rawLevels[level][i], LinearByte(lower) - 1e-6f);
                        EXPECT_LE(rawLevels[level][i], LinearByte(upper) + 1e-6f);
                    }
                }
            }
        }
        m_Textures.Evict(guid);
        const auto adopted = WaitForUpload(guid);
        ASSERT_TRUE(adopted.IsValid());
        for (uint32 level = 0, extent = 64; level < 7; ++level, extent = std::max(extent / 2, 1u))
            EXPECT_EQ(Render(adopted, extent, extent, float(level)), rawLevels[level]);
    }
}

#endif

TEST_F(TextureCoverageGPU, ANewTextureServiceStreamsMaterialMipsUnderThePlatformBudget)
{
    using Access = Engine::Renderer::TextureCoverageUploadTestAccess;
    const uint64 budget = Engine::Renderer::MaterialMipBudget::ForPlatform(m_Device->GetCapabilities());
    ASSERT_NE(budget, 0u);
    EXPECT_EQ(Access::MipBudget(m_Textures), budget);
    EXPECT_TRUE(m_Textures.StreamsMaterialMips());
}

TEST_F(TextureCoverageGPU, MaterialMipDemandPublishesOnlyAfterTheMainCameraWorldSweeps)
{
    using Engine::Renderer::MaterialMipDemandSystem;
    using Engine::Renderer::RenderServices;
    using Engine::Renderer::TextureService;
    using Access = Engine::Renderer::TextureCoverageUploadTestAccess;

    RenderServices services;
    struct Shutdown
    {
        RenderServices& Services;
        ~Shutdown() { Services.Shutdown(); }
    } shutdown{services};
    ASSERT_TRUE(services.Initialize(m_Device.get()));
    TextureService& textures = services.Textures();
    Access::SetMipBudget(textures, 2u * 1024u * 1024u);
    MaterialMipDemandSystem demand(&services);
    ASSERT_TRUE(demand.IsEnabled());

    ECS::World mainWorld, otherWorld;
    auto& views = services.Views();
    const ViewId mainView = AddMipDemandView(views, "MipDemand.Main", mainWorld);
    AddMipDemandView(views, "MipDemand.Other", otherWorld);

    GPUInstance instance{};
    instance.materialIndex = 1;
    instance.boundingCenter = {0.0f, 0.0f, 5.0f};
    instance.boundingRadius = 2.0f;
    services.GetGPUScene()->AddInstance(instance);

    constexpr int kUpdatesPerSweep = 30;
    for (int update = 0; update < kUpdatesPerSweep; ++update)
        demand.Update(otherWorld, 0.0f);
    EXPECT_TRUE(Access::MipCoverage(textures).empty());

    for (int update = 1; update < kUpdatesPerSweep; ++update)
        demand.Update(mainWorld, 0.0f);
    EXPECT_TRUE(Access::MipCoverage(textures).empty());
    demand.Update(mainWorld, 0.0f);
    const auto coverage = Access::MipCoverage(textures);
    ASSERT_EQ(coverage.size(), 2u);
    EXPECT_FLOAT_EQ(coverage[0], 0.0f);
    EXPECT_FLOAT_EQ(coverage[1], 0.5f);

    const CameraId pendingCamera = views.AllocateCamera("MipDemand.Pending");
    views.SetViewCamera(mainView, pendingCamera);
    demand.Update(mainWorld, 0.0f);
    EXPECT_TRUE(Access::MipCoverage(textures).empty());
}

TEST_F(TextureCoverageGPU, MaterialMipStreamingRespectsBudgetAndKeepsDirectTextureSeparate)
{
    using Access = Engine::Renderer::TextureCoverageUploadTestAccess;
    constexpr uint64 kMiB = 1024 * 1024;
    Access::SetMipBudget(m_Textures, kMiB);
    GUID texture;
    SharedPtr<TextureAsset> asset;
    ASSERT_NO_FATAL_FAILURE(MakeStreamedTexture(texture, asset));

    const GUID materialGuid = GUID::Generate();
    MaterialDocument doc{};
    doc.schemaVersion = 3;
    doc.materialName = "MipStreaming";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "Surfaces/standard_pbr.glsl";
    auto* material = m_Materials.Register(materialGuid, doc);
    ASSERT_NE(material, nullptr);
    const StringId slot = HashStringId("albedoMap");
    m_Textures.TrackMaterialTextureRefForTesting(materialGuid, slot, texture);
    Access::Queue(m_Textures, texture, materialGuid, asset, slot);
    m_Textures.FlushPendingUploads();

    const auto tail = Access::StreamedHandle(m_Textures, texture);
    ASSERT_TRUE(tail.IsValid());
    EXPECT_EQ(material->GetTexture(slot), tail);
    uint32 width = 0, height = 0;
    m_Device->GetTextureSize(tail, width, height);
    EXPECT_EQ(width, 256u);
    EXPECT_EQ(height, 256u);
    m_Textures.FlushPendingUploads();
    EXPECT_EQ(material->GetTexture(slot), tail);
    // A read of the material image starts no direct upload.
    EXPECT_EQ(m_Textures.GetMaterialTexture(texture), tail);
    m_Textures.FlushPendingUploads();
    EXPECT_FALSE(Access::Uploaded(m_Textures, texture));

    const auto direct = m_Textures.GetOrUpload(texture);
    ASSERT_TRUE(direct.IsValid());
    EXPECT_NE(direct, tail);
    m_Device->GetTextureSize(direct, width, height);
    EXPECT_EQ(width, 512u);
    EXPECT_EQ(height, 512u);
    EXPECT_EQ(Render(tail, 16, 16, 0), Render(direct, 16, 16, 1));

    Access::SetMipBudget(m_Textures, 2 * kMiB);
    SettleMipStreams();
    const auto promoted = material->GetTexture(slot);
    EXPECT_NE(promoted, tail);
    EXPECT_NE(promoted, direct);
    m_Device->GetTextureSize(promoted, width, height);
    EXPECT_EQ(width, 512u);
    EXPECT_EQ(height, 512u);
    EXPECT_EQ(Render(promoted, 16, 16, 0), Render(direct, 16, 16, 0));

    // An override uses a direct image while the authored texture stays tracked.
    m_Textures.BindMaterialTexture(material, slot, direct);
    Access::SetMipBudget(m_Textures, kMiB);
    SettleMipStreams();
    EXPECT_EQ(material->GetTexture(slot), direct);
    m_Textures.RestoreMaterialTextureBinding(material, materialGuid, slot);
    const auto demoted = material->GetTexture(slot);
    ASSERT_TRUE(demoted.IsValid());
    EXPECT_NE(demoted, direct);
    m_Device->GetTextureSize(demoted, width, height);
    EXPECT_EQ(width, 256u);
    EXPECT_EQ(height, 256u);
    EXPECT_EQ(m_Textures.GetOrUpload(texture), direct);

    m_Textures.OnMaterialUnregistered(materialGuid);
    m_Materials.Unregister(materialGuid);
    m_Textures.FlushPendingUploads();
    EXPECT_FALSE(Access::StreamedHandle(m_Textures, texture).IsValid());
    EXPECT_EQ(m_Textures.GetOrUpload(texture), direct);
}

TEST_F(TextureCoverageGPU, MaterialMipStreamingGivesDetailToTheMaterialWithTheMostScreenCoverage)
{
    using Access = Engine::Renderer::TextureCoverageUploadTestAccess;
    constexpr uint64 kMiB = 1024 * 1024;
    // Two tails and one level 0 fit. Two level 0 images do not.
    Access::SetMipBudget(m_Textures, 2 * kMiB);

    struct Streamed
    {
        GUID Texture;
        GUID MaterialGuid;
        Engine::Renderer::Material* BoundMaterial = nullptr;
        uint32 MaterialIndex = 0;
    };
    std::array<Streamed, 2> streamed;
    const StringId slot = HashStringId("albedoMap");
    for (uint32 index = 0; index < streamed.size(); ++index)
    {
        Streamed& entry = streamed[index];
        SharedPtr<TextureAsset> asset;
        ASSERT_NO_FATAL_FAILURE(MakeStreamedTexture(entry.Texture, asset));
        entry.MaterialGuid = GUID::Generate();
        entry.MaterialIndex = index;
        MaterialDocument doc{};
        doc.schemaVersion = 3;
        doc.materialName = "MipStreamingDemand";
        doc.lightingModel = "StandardPBR";
        doc.surfaceShader = "Surfaces/standard_pbr.glsl";
        entry.BoundMaterial = m_Materials.Register(entry.MaterialGuid, doc);
        ASSERT_NE(entry.BoundMaterial, nullptr);
        Engine::Renderer::Material::TestFactory::SetGpuSceneMaterialIndex(*entry.BoundMaterial, index);
        m_Textures.TrackMaterialTextureRefForTesting(entry.MaterialGuid, slot, entry.Texture);
        Access::Queue(m_Textures, entry.Texture, entry.MaterialGuid, asset, slot);
    }
    // The texture that loads first starts at level 0: the demand goes to the second one.
    std::array<float, 2> coverage{};
    coverage[streamed[0].MaterialIndex] = 0.01f;
    coverage[streamed[1].MaterialIndex] = 0.5f;
    m_Textures.UpdateMaterialMipDemand(coverage);
    SettleMipStreams();
    EXPECT_EQ(WidthOf(streamed[0].BoundMaterial->GetTexture(slot)), 256u);
    EXPECT_EQ(WidthOf(streamed[1].BoundMaterial->GetTexture(slot)), 512u);
    const std::array<GUID, 2> textures{streamed[0].Texture, streamed[1].Texture};
    ExpectMipStreamReport(m_Textures, textures, std::array<uint32, 2>{1u, 0u}, coverage);
    // With no change to the demand, the streams or the budget, the planner does not run again.
    EXPECT_TRUE(Access::MipStreamsSettled(m_Textures));
    m_Textures.FlushPendingUploads();
    EXPECT_TRUE(Access::MipStreamsSettled(m_Textures));

    // The demand moves: the first texture takes the level that the second one held.
    coverage[streamed[0].MaterialIndex] = 0.9f;
    coverage[streamed[1].MaterialIndex] = 0.01f;
    m_Textures.UpdateMaterialMipDemand(coverage);
    SettleMipStreams();
    EXPECT_EQ(WidthOf(streamed[0].BoundMaterial->GetTexture(slot)), 512u);
    EXPECT_EQ(WidthOf(streamed[1].BoundMaterial->GetTexture(slot)), 256u);
    ExpectMipStreamReport(m_Textures, textures, std::array<uint32, 2>{0u, 1u}, coverage);

    for (const Streamed& entry : streamed)
    {
        m_Textures.OnMaterialUnregistered(entry.MaterialGuid);
        m_Materials.Unregister(entry.MaterialGuid);
    }
    m_Textures.FlushPendingUploads();
}

TEST_F(TextureCoverageGPU, ANewMaterialMipStreamStartsAtTheLargestLevelThatFitsTheBudget)
{
    using Access = Engine::Renderer::TextureCoverageUploadTestAccess;
    constexpr uint64 kMiB = 1024 * 1024;
    // The levels from 512 texels a side are about 1.33 MiB, and the levels from 256 about 0.33 MiB.
    // The first texture, 1024 texels a side, fits from its level 1. The second then fits at its tail only.
    Access::SetMipBudget(m_Textures, 2 * kMiB);
    const StringId slot = HashStringId("albedoMap");
    constexpr std::array<int, 2> kSizes{1024, 512};
    std::array<GUID, 2> materialGuids;
    std::array<Engine::Renderer::Material*, 2> materials{};
    for (size_t index = 0; index < materials.size(); ++index)
    {
        GUID texture;
        SharedPtr<TextureAsset> asset;
        ASSERT_NO_FATAL_FAILURE(MakeStreamedTexture(texture, asset, kSizes[index]));
        materialGuids[index] = GUID::Generate();
        materials[index] = RegisterStreamingMaterial(materialGuids[index], "MipStreamingFirstLevel");
        ASSERT_NE(materials[index], nullptr);
        m_Textures.TrackMaterialTextureRefForTesting(materialGuids[index], slot, texture);
        Access::Queue(m_Textures, texture, materialGuids[index], asset, slot);
    }
    // One flush stages both, with no step: each level is the level of the first upload.
    m_Textures.FlushPendingUploads();
    EXPECT_EQ(WidthOf(materials[0]->GetTexture(slot)), 512u) << "the stream did not start at the largest level that fits";
    EXPECT_EQ(WidthOf(materials[1]->GetTexture(slot)), 256u)
        << "the stream did not count the bytes that the same flush staged";

    for (const GUID& materialGuid : materialGuids)
    {
        m_Textures.OnMaterialUnregistered(materialGuid);
        m_Materials.Unregister(materialGuid);
    }
    m_Textures.FlushPendingUploads();
}

TEST_F(TextureCoverageGPU, ANewMaterialMipStreamReservesAnInFlightGain)
{
    using Access = Engine::Renderer::TextureCoverageUploadTestAccess;
    constexpr uint64 kMiB = 1024 * 1024;
    constexpr uint32 kFullWidth = 512;
    constexpr uint32 kTailWidth = 256;
    const StringId slot = HashStringId("albedoMap");
    Access::SetMipBudget(m_Textures, kMiB);

    GUID firstTexture;
    SharedPtr<TextureAsset> firstAsset;
    ASSERT_NO_FATAL_FAILURE(MakeStreamedTexture(firstTexture, firstAsset, kFullWidth));
    const GUID firstMaterialGuid = GUID::Generate();
    auto* firstMaterial = RegisterStreamingMaterial(firstMaterialGuid, "MipStreamingGain");
    ASSERT_NE(firstMaterial, nullptr);
    m_Textures.TrackMaterialTextureRefForTesting(firstMaterialGuid, slot, firstTexture);
    Access::Queue(m_Textures, firstTexture, firstMaterialGuid, firstAsset, slot);
    m_Textures.FlushPendingUploads();
    ASSERT_EQ(WidthOf(firstMaterial->GetTexture(slot)), kTailWidth);

    GUID secondTexture;
    SharedPtr<TextureAsset> secondAsset;
    ASSERT_NO_FATAL_FAILURE(MakeStreamedTexture(secondTexture, secondAsset, kFullWidth));
    const GUID secondMaterialGuid = GUID::Generate();
    auto* secondMaterial = RegisterStreamingMaterial(secondMaterialGuid, "MipStreamingDuringGain");
    ASSERT_NE(secondMaterial, nullptr);
    m_Textures.TrackMaterialTextureRefForTesting(secondMaterialGuid, slot, secondTexture);
    Access::Queue(m_Textures, secondTexture, secondMaterialGuid, secondAsset, slot);

    // One flush begins the gain read before admitting the new stream.
    // Two full chains exceed 2 MiB. A full chain and a tail fit.
    Access::SetMipBudget(m_Textures, 2 * kMiB);
    m_Textures.FlushPendingUploads();
    EXPECT_EQ(WidthOf(firstMaterial->GetTexture(slot)), kTailWidth);
    EXPECT_EQ(WidthOf(secondMaterial->GetTexture(slot)), kTailWidth);

    SettleMipStreams();
    EXPECT_EQ(WidthOf(firstMaterial->GetTexture(slot)), kFullWidth);
    EXPECT_EQ(WidthOf(secondMaterial->GetTexture(slot)), kTailWidth);

    for (const GUID& materialGuid : {firstMaterialGuid, secondMaterialGuid})
    {
        m_Textures.OnMaterialUnregistered(materialGuid);
        m_Materials.Unregister(materialGuid);
    }
    m_Textures.FlushPendingUploads();
}

TEST_F(TextureCoverageGPU, AnEvictedStreamInsideTheBudgetStartsAgainAtFullResolutionFromItsFile)
{
    using Access = Engine::Renderer::TextureCoverageUploadTestAccess;
    constexpr uint64 kMiB = 1024 * 1024;
    Access::SetMipBudget(m_Textures, 2 * kMiB);
    const GUID texture = Register(Image(false, 512), false);
    ASSERT_FALSE(texture.IsNull());
    const GUID materialGuid = GUID::Generate();
    auto* material = RegisterStreamingMaterial(materialGuid, "MipStreamingEvictedFull");
    ASSERT_NE(material, nullptr);
    const StringId slot = HashStringId("albedoMap");
    m_Textures.TrackMaterialTextureRefForTesting(materialGuid, slot, texture);
    m_Textures.ScheduleMaterialTextureLoad(texture, materialGuid, slot);
    const auto first = WaitForBoundTexture(*material, slot);
    ASSERT_EQ(WidthOf(first), 512u);
    const auto loaded =
        std::dynamic_pointer_cast<TextureAsset>(EngineCore::GetInstance().GetAssetManager().GetAsset(texture));
    ASSERT_NE(loaded, nullptr);
    ASSERT_EQ(loaded->GetPixelData(), nullptr);

    // The asset stays loaded with no pixels: the new stream reads all its levels from the file.
    m_Textures.Evict(texture);
    const auto second = WaitForBoundTexture(*material, slot);
    ASSERT_EQ(WidthOf(second), 512u);
    EXPECT_NE(second, first);
    EXPECT_EQ(loaded->GetPixelData(), nullptr);

    m_Textures.OnMaterialUnregistered(materialGuid);
    m_Materials.Unregister(materialGuid);
    m_Textures.FlushPendingUploads();
}

TEST_F(TextureCoverageGPU, TheMemoryStatsReportTheBytesThatTheMaterialMipStreamsHold)
{
    using Access = Engine::Renderer::TextureCoverageUploadTestAccess;
    constexpr uint64 kMiB = 1024 * 1024;
    // RGBA8 levels of 512 texels a side and smaller, and of 256 texels a side and smaller.
    constexpr uint64 kFullBytes = 1398100;
    constexpr uint64 kTailBytes = 349524;
    const uint64 before = StreamedBytesInMemoryStats();
    Access::SetMipBudget(m_Textures, kMiB);
    GUID texture;
    SharedPtr<TextureAsset> asset;
    ASSERT_NO_FATAL_FAILURE(MakeStreamedTexture(texture, asset));
    const GUID materialGuid = GUID::Generate();
    auto* material = RegisterStreamingMaterial(materialGuid, "MipStreamingStats");
    ASSERT_NE(material, nullptr);
    const StringId slot = HashStringId("albedoMap");
    m_Textures.TrackMaterialTextureRefForTesting(materialGuid, slot, texture);
    Access::Queue(m_Textures, texture, materialGuid, asset, slot);
    m_Textures.FlushPendingUploads();
    ASSERT_EQ(WidthOf(material->GetTexture(slot)), 256u);
    EXPECT_EQ(StreamedBytesInMemoryStats() - before, kTailBytes);

    Access::SetMipBudget(m_Textures, 2 * kMiB);
    SettleMipStreams();
    ASSERT_EQ(WidthOf(material->GetTexture(slot)), 512u);
    EXPECT_EQ(StreamedBytesInMemoryStats() - before, kFullBytes);

    // An evict ends the stream at once.
    m_Textures.Evict(texture);
    EXPECT_EQ(StreamedBytesInMemoryStats(), before);

    m_Textures.OnMaterialUnregistered(materialGuid);
    m_Materials.Unregister(materialGuid);
    m_Textures.FlushPendingUploads();
    EXPECT_EQ(StreamedBytesInMemoryStats(), before);
}

TEST_F(TextureCoverageGPU, MaterialMipStreamKeepsNoPixelsAndReadsItsLevelsFromTheFile)
{
    using Access = Engine::Renderer::TextureCoverageUploadTestAccess;
    constexpr uint64 kMiB = 1024 * 1024;
    Access::SetMipBudget(m_Textures, kMiB);
    GUID texture;
    SharedPtr<TextureAsset> asset;
    ASSERT_NO_FATAL_FAILURE(MakeStreamedTexture(texture, asset));
    const GUID materialGuid = GUID::Generate();
    auto* material = RegisterStreamingMaterial(materialGuid, "MipStreamingFromFile");
    ASSERT_NE(material, nullptr);
    const StringId slot = HashStringId("albedoMap");
    m_Textures.TrackMaterialTextureRefForTesting(materialGuid, slot, texture);
    Access::Queue(m_Textures, texture, materialGuid, asset, slot);
    m_Textures.FlushPendingUploads();
    ASSERT_EQ(WidthOf(material->GetTexture(slot)), 256u);
    // The stream keeps no copy of the mip chain, and the asset gave its copy up.
    EXPECT_EQ(asset->GetPixelData(), nullptr);

    Access::SetMipBudget(m_Textures, 2 * kMiB);
    SettleMipStreams();
    const auto promoted = material->GetTexture(slot);
    ASSERT_EQ(WidthOf(promoted), 512u);
    // The reference: a direct upload loads the whole file into an asset of its own.
    const auto direct = m_Textures.GetOrUpload(texture);
    ASSERT_TRUE(direct.IsValid());
    EXPECT_EQ(Render(promoted, 16, 16, 0), Render(direct, 16, 16, 0));
    EXPECT_EQ(Render(promoted, 16, 16, 3), Render(direct, 16, 16, 3));

    m_Textures.OnMaterialUnregistered(materialGuid);
    m_Materials.Unregister(materialGuid);
    m_Textures.FlushPendingUploads();
}

TEST_F(TextureCoverageGPU, MaterialMipStreamOfACookedSourceReleasesThePixelsOfTheLoadedAsset)
{
    using Access = Engine::Renderer::TextureCoverageUploadTestAccess;
    constexpr uint64 kMiB = 1024 * 1024;
    Access::SetMipBudget(m_Textures, kMiB);
    // A source image: the AssetManager cooks it, and the stream reads the artifact in the cook cache.
    const GUID texture = Register(Image(false, 512), false);
    ASSERT_FALSE(texture.IsNull());

    const GUID materialGuid = GUID::Generate();
    auto* material = RegisterStreamingMaterial(materialGuid, "MipStreamingCookedSource");
    ASSERT_NE(material, nullptr);
    const StringId slot = HashStringId("albedoMap");
    m_Textures.TrackMaterialTextureRefForTesting(materialGuid, slot, texture);
    m_Textures.ScheduleMaterialTextureLoad(texture, materialGuid, slot);
    auto& assets = EngineCore::GetInstance().GetAssetManager();
    ASSERT_EQ(WidthOf(WaitForBoundTexture(*material, slot)), 256u);
    const auto loaded = std::dynamic_pointer_cast<TextureAsset>(assets.GetAsset(texture));
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->GetPixelData(), nullptr);

    Access::SetMipBudget(m_Textures, 2 * kMiB);
    SettleMipStreams();
    const auto streamed = material->GetTexture(slot);
    EXPECT_EQ(WidthOf(streamed), 512u);

    // A CPU reader of the shared asset borrows the pixels from the artifact in the cook cache.
    // The asset keeps none.
    {
        const std::shared_ptr<const uint8[]> held = loaded->AcquirePixels().Pixels;
        ASSERT_NE(held, nullptr);
        EXPECT_EQ(loaded->GetPixelData(), nullptr);
    }

    // A direct upload of the same asset reads all levels from the artifact, and the asset stays released.
    const auto direct = WaitForUpload(texture);
    ASSERT_TRUE(direct.IsValid());
    EXPECT_NE(direct, streamed);
    EXPECT_EQ(WidthOf(direct), 512u);
    EXPECT_EQ(loaded->GetPixelData(), nullptr);
    EXPECT_EQ(Render(direct, 16, 16, 0), Render(streamed, 16, 16, 0));

    m_Textures.OnMaterialUnregistered(materialGuid);
    m_Materials.Unregister(materialGuid);
    m_Textures.FlushPendingUploads();
}

TEST_F(TextureCoverageGPU, AStreamedMaterialTextureAlsoUploadsAsAUiBackground)
{
    using Access = Engine::Renderer::TextureCoverageUploadTestAccess;
    constexpr uint64 kMiB = 1024 * 1024;
    constexpr uint32 kFullWidth = 512;
    constexpr uint32 kTailWidth = kFullWidth / 2;
    constexpr uint32 kProbeWidth = 16;
    Access::SetMipBudget(m_Textures, kMiB);
    const GUID texture = Register(Image(false, kFullWidth), false);
    ASSERT_FALSE(texture.IsNull());

    const GUID materialGuid = GUID::Generate();
    auto* material = RegisterStreamingMaterial(materialGuid, "MipStreamingUiBackground");
    ASSERT_NE(material, nullptr);
    const StringId slot = HashStringId("albedoMap");
    m_Textures.TrackMaterialTextureRefForTesting(materialGuid, slot, texture);
    m_Textures.ScheduleMaterialTextureLoad(texture, materialGuid, slot);
    const auto tail = WaitForBoundTexture(*material, slot);
    ASSERT_EQ(WidthOf(tail), kTailWidth);

    auto& assets = EngineCore::GetInstance().GetAssetManager();
    const auto loaded = std::dynamic_pointer_cast<TextureAsset>(assets.GetAsset(texture));
    ASSERT_NE(loaded, nullptr);
    ASSERT_EQ(loaded->GetPixelData(), nullptr);

    UIManager ui(m_Device.get(), &assets);
    ui.SetRoot(std::make_unique<UIElement>());
    uint32 width = 0;
    uint32 height = 0;
    const auto background = ui.BackgroundImages().TryGetHandle(texture, &width, &height);
    ASSERT_TRUE(background.IsValid());
    EXPECT_EQ(width, kFullWidth);
    EXPECT_EQ(height, kFullWidth);
    EXPECT_EQ(WidthOf(background), kFullWidth);
    EXPECT_EQ(ui.BackgroundImages().RasterMipLevelsForTesting(texture),
              ComputeTextureCookMipCount(kFullWidth, kFullWidth, TextureCookSettings{}));
    EXPECT_EQ(material->GetTexture(slot), tail);
    EXPECT_EQ(loaded->GetPixelData(), nullptr);

    const auto backgroundPixels = Render(background, kProbeWidth, kProbeWidth, 1);
    ASSERT_FALSE(backgroundPixels.empty());
    EXPECT_EQ(backgroundPixels, Render(tail, kProbeWidth, kProbeWidth, 0));

    m_Textures.OnMaterialUnregistered(materialGuid);
    m_Materials.Unregister(materialGuid);
    m_Textures.FlushPendingUploads();
}

TEST_F(TextureCoverageGPU, AMaterialMipStreamOfABlockCompressedTextureMatchesTheDirectUpload)
{
    CheckBlockCompressedMipStream(Image(false, 512), "bc7", GameEngine::TextureFormat::BC7, 128 * 1024);
}

TEST_F(TextureCoverageGPU, AStreamedBC1TextureMatchesItsDirectUpload)
{
    CheckBlockCompressedMipStream(Image(false, 512), "bc1", GameEngine::TextureFormat::BC1, 64 * 1024);
}

TEST_F(TextureCoverageGPU, AStreamedBC4TextureMatchesItsDirectUpload)
{
    CheckBlockCompressedMipStream(Image(false, 512), "bc4", GameEngine::TextureFormat::BC4, 64 * 1024);
}

TEST_F(TextureCoverageGPU, AStreamedBC6HTextureKeepsItsHdrRangeAndMatchesItsDirectUpload)
{
    constexpr uint32 kWidth = 512;
    constexpr uint32 kHeight = 16;
    // The long edge still requires a mip tail. The short edge limits BC6H encoder work.
    const auto path = s_Workspace / "Assets" / (GUID::Generate().ToString() + ".hdr");
    const auto data = GameEngine::Tests::MakeGradientHdr(kWidth, kHeight);
    {
        std::ofstream stream(path, std::ios::binary);
        stream.write(reinterpret_cast<const char*>(data.data()), data.size());
        ASSERT_TRUE(stream.good());
    }
    CheckBlockCompressedMipStream(path, "bc6h", GameEngine::TextureFormat::BC6H, 8 * 1024, kHeight);
}

TEST_F(TextureCoverageGPU, AnEvictedStreamOfAReleasedTextureStartsAgainFromItsFile)
{
    using Access = Engine::Renderer::TextureCoverageUploadTestAccess;
    constexpr uint64 kMiB = 1024 * 1024;
    Access::SetMipBudget(m_Textures, kMiB);
    const GUID texture = Register(Image(false, 512), false);
    ASSERT_FALSE(texture.IsNull());
    const GUID materialGuid = GUID::Generate();
    auto* material = RegisterStreamingMaterial(materialGuid, "MipStreamingEvicted");
    ASSERT_NE(material, nullptr);
    const StringId slot = HashStringId("albedoMap");
    m_Textures.TrackMaterialTextureRefForTesting(materialGuid, slot, texture);
    m_Textures.ScheduleMaterialTextureLoad(texture, materialGuid, slot);
    const auto first = WaitForBoundTexture(*material, slot);
    ASSERT_EQ(WidthOf(first), 256u);
    const auto loaded =
        std::dynamic_pointer_cast<TextureAsset>(EngineCore::GetInstance().GetAssetManager().GetAsset(texture));
    ASSERT_NE(loaded, nullptr);
    ASSERT_EQ(loaded->GetPixelData(), nullptr);

    // The file keeps its header and its tail only: a read of the whole file fails.
    const TextureLevelFile* file = loaded->GetLevelFile();
    ASSERT_NE(file, nullptr);
    constexpr uint32 kTailLevel = 1; // 256 texels a side
    std::filesystem::resize_file(file->Path(), file->PrefixBytes(kTailLevel));

    // The evict ends the stream. The asset stays loaded with no pixels: the new stream reads its
    // tail from the file, and the asset gets no pixels back.
    m_Textures.Evict(texture);
    const auto second = WaitForBoundTexture(*material, slot);
    ASSERT_EQ(WidthOf(second), 256u);
    EXPECT_NE(second, first);
    EXPECT_EQ(loaded->GetPixelData(), nullptr);

    m_Textures.OnMaterialUnregistered(materialGuid);
    m_Materials.Unregister(materialGuid);
    m_Textures.FlushPendingUploads();
}

TEST_F(TextureCoverageGPU, AHotSwapOfAStreamedTextureBindsTheFullTextureFromItsFile)
{
    using Access = Engine::Renderer::TextureCoverageUploadTestAccess;
    constexpr uint64 kMiB = 1024 * 1024;
    Access::SetMipBudget(m_Textures, kMiB);
    const GUID texture = Register(Image(false, 512), false);
    ASSERT_FALSE(texture.IsNull());
    const GUID materialGuid = GUID::Generate();
    auto* material = RegisterStreamingMaterial(materialGuid, "MipStreamingHotSwap");
    ASSERT_NE(material, nullptr);
    const StringId slot = HashStringId("albedoMap");
    m_Textures.TrackMaterialTextureRefForTesting(materialGuid, slot, texture);
    m_Textures.ScheduleMaterialTextureLoad(texture, materialGuid, slot);
    const auto streamed = WaitForBoundTexture(*material, slot);
    ASSERT_EQ(WidthOf(streamed), 256u);
    const auto loaded =
        std::dynamic_pointer_cast<TextureAsset>(EngineCore::GetInstance().GetAssetManager().GetAsset(texture));
    ASSERT_NE(loaded, nullptr);
    ASSERT_EQ(loaded->GetPixelData(), nullptr);

    // The swap gives a result at once: it reads the released pixels again and uploads all levels.
    Access::HotSwap(m_Textures, texture);
    const auto swapped = material->GetTexture(slot);
    ASSERT_TRUE(swapped.IsValid());
    EXPECT_NE(swapped, streamed);
    EXPECT_EQ(WidthOf(swapped), 512u);
    EXPECT_TRUE(Access::Uploaded(m_Textures, texture));
    EXPECT_FALSE(Access::StreamedHandle(m_Textures, texture).IsValid());

    m_Textures.OnMaterialUnregistered(materialGuid);
    m_Materials.Unregister(materialGuid);
    m_Textures.FlushPendingUploads();
}

TEST_F(TextureCoverageGPU, ANormalMapStreamReadsItsVarianceCompanionFromItsFile)
{
    using Access = Engine::Renderer::TextureCoverageUploadTestAccess;
    constexpr uint64 kKiB = 1024;
    constexpr uint64 kFullBudget = 1024 * kKiB;
    constexpr uint32 kFullWidth = 512;
    struct AdmissionCase
    {
        uint64 Budget;
        uint32 FirstWidth;
    };
    // At 368 KiB the full BC5 chain fits alone. The variance companion makes it exceed the budget.
    constexpr std::array kCases{AdmissionCase{368 * kKiB, kFullWidth / 2},
                                AdmissionCase{kFullBudget, kFullWidth}};

    for (const AdmissionCase& admission : kCases)
    {
        SCOPED_TRACE(admission.Budget);
        Access::SetMipBudget(m_Textures, admission.Budget);
        const auto path = Image(false, kFullWidth);
        Set(path, kTextureUsageMetaKey, "normal");
        Set(path, kTextureColorSpaceMetaKey, "linear");
        auto& assets = EngineCore::GetInstance().GetAssetManager();
        const GUID texture = assets.GetRegistry().GetAssetGUID(path);
        ASSERT_FALSE(texture.IsNull());
        const GUID materialGuid = GUID::Generate();
        auto* material = RegisterStreamingMaterial(materialGuid, "MipStreamingNormalMap");
        ASSERT_NE(material, nullptr);
        const StringId slot = HashStringId("normalMap");
        m_Textures.TrackMaterialTextureRefForTesting(materialGuid, slot, texture);
        m_Textures.ScheduleMaterialTextureLoad(texture, materialGuid, slot);
        const auto first = WaitForBoundTexture(*material, slot);
        ASSERT_TRUE(first.IsValid());
        const auto loaded = std::dynamic_pointer_cast<TextureAsset>(assets.GetAsset(texture));
        ASSERT_NE(loaded, nullptr);
        if (loaded->GetFormat() != GameEngine::TextureFormat::BC5)
            GTEST_SKIP() << "the cook made no BC5 normal map on this device";
        const auto* variance = loaded->GetNormalVarianceCompanion();
        ASSERT_NE(variance, nullptr);
        const auto* normalFile = loaded->GetLevelFile();
        const auto* varianceFile = variance->GetLevelFile();
        ASSERT_NE(normalFile, nullptr);
        ASSERT_NE(varianceFile, nullptr);
        const uint64 normalBytes = LevelPayloadBytes(*normalFile);
        const uint64 combinedBytes = normalBytes + LevelPayloadBytes(*varianceFile);
        EXPECT_LE(normalBytes, admission.Budget);
        EXPECT_EQ(combinedBytes <= admission.Budget, admission.FirstWidth == kFullWidth);
        ASSERT_EQ(WidthOf(first), admission.FirstWidth);
        const auto firstCompanion = Access::StreamedCompanion(m_Textures, texture);
        ASSERT_TRUE(firstCompanion.IsValid());
        // The companion holds the variance of the next smaller mip.
        EXPECT_EQ(WidthOf(firstCompanion), admission.FirstWidth / 2);
        EXPECT_EQ(loaded->GetPixelData(), nullptr);
        EXPECT_EQ(variance->GetPixelData(), nullptr);

        if (admission.FirstWidth < kFullWidth)
        {
            Access::SetMipBudget(m_Textures, kFullBudget);
            SettleMipStreams();
            EXPECT_EQ(WidthOf(material->GetTexture(slot)), kFullWidth);
            const auto fullCompanion = Access::StreamedCompanion(m_Textures, texture);
            ASSERT_TRUE(fullCompanion.IsValid());
            EXPECT_NE(fullCompanion, firstCompanion);
            EXPECT_EQ(WidthOf(fullCompanion), kFullWidth / 2);
        }

        m_Textures.OnMaterialUnregistered(materialGuid);
        m_Materials.Unregister(materialGuid);
        m_Textures.FlushPendingUploads();
    }
}

TEST_F(TextureCoverageGPU, AnEvictDuringAStepReadDropsTheReadAndTheNextStreamSettles)
{
    using Access = Engine::Renderer::TextureCoverageUploadTestAccess;
    constexpr uint64 kMiB = 1024 * 1024;
    Access::SetMipBudget(m_Textures, kMiB);
    const GUID texture = Register(Image(false, 512), false);
    ASSERT_FALSE(texture.IsNull());
    const GUID materialGuid = GUID::Generate();
    auto* material = RegisterStreamingMaterial(materialGuid, "MipStreamingEvictedInAStep");
    ASSERT_NE(material, nullptr);
    const StringId slot = HashStringId("albedoMap");
    m_Textures.TrackMaterialTextureRefForTesting(materialGuid, slot, texture);
    m_Textures.ScheduleMaterialTextureLoad(texture, materialGuid, slot);
    ASSERT_EQ(WidthOf(WaitForBoundTexture(*material, slot)), 256u);
    const auto loaded =
        std::dynamic_pointer_cast<TextureAsset>(EngineCore::GetInstance().GetAssetManager().GetAsset(texture));
    ASSERT_NE(loaded, nullptr);

    // The flush starts the read of level 0. The evict comes before the flush that finishes it.
    Access::SetMipBudget(m_Textures, 2 * kMiB);
    m_Textures.FlushPendingUploads();
    ASSERT_FALSE(Access::MipStreamsSettled(m_Textures));
    m_Textures.Evict(texture);
    ASSERT_TRUE(WaitForBoundTexture(*material, slot).IsValid());
    SettleMipStreams();
    EXPECT_EQ(WidthOf(material->GetTexture(slot)), 512u);
    EXPECT_EQ(loaded->GetPixelData(), nullptr);

    m_Textures.OnMaterialUnregistered(materialGuid);
    m_Materials.Unregister(materialGuid);
    m_Textures.FlushPendingUploads();
}

TEST_F(TextureCoverageGPU, MaterialMipStreamKeepsItsLevelWhenItsFileIsAnotherFile)
{
    using Access = Engine::Renderer::TextureCoverageUploadTestAccess;
    constexpr uint64 kMiB = 1024 * 1024;
    Access::SetMipBudget(m_Textures, kMiB);
    GUID texture;
    SharedPtr<TextureAsset> asset;
    ASSERT_NO_FATAL_FAILURE(MakeStreamedTexture(texture, asset));

    const GUID materialGuid = GUID::Generate();
    auto* material = RegisterStreamingMaterial(materialGuid, "MipStreamingReplacedFile");
    ASSERT_NE(material, nullptr);
    const StringId slot = HashStringId("albedoMap");
    m_Textures.TrackMaterialTextureRefForTesting(materialGuid, slot, texture);
    Access::Queue(m_Textures, texture, materialGuid, asset, slot);
    m_Textures.FlushPendingUploads();
    const auto tail = material->GetTexture(slot);
    ASSERT_EQ(WidthOf(tail), 256u);

    // The budget gets room for level 0. The read of it fails, and the stream does not ask again.
    ASSERT_NO_FATAL_FAILURE(ReplaceFileOf(*asset));
    Access::SetMipBudget(m_Textures, 2 * kMiB);
    SettleMipStreams();
    EXPECT_EQ(material->GetTexture(slot), tail);
    m_Textures.FlushPendingUploads();
    EXPECT_TRUE(Access::MipStreamsSettled(m_Textures));
    EXPECT_EQ(material->GetTexture(slot), tail);

    m_Textures.OnMaterialUnregistered(materialGuid);
    m_Materials.Unregister(materialGuid);
    m_Textures.FlushPendingUploads();
}

TEST_F(TextureCoverageGPU, FailedDirectTextureLoadKeepsItsMaterialStream)
{
    using Access = Engine::Renderer::TextureCoverageUploadTestAccess;
    constexpr uint64 kMiB = 1024 * 1024;
    Access::SetMipBudget(m_Textures, kMiB);
    GUID texture;
    SharedPtr<TextureAsset> asset;
    ASSERT_NO_FATAL_FAILURE(MakeStreamedTexture(texture, asset));

    const GUID materialGuid = GUID::Generate();
    MaterialDocument doc{};
    doc.schemaVersion = 3;
    doc.materialName = "MipStreamingFailure";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "Surfaces/standard_pbr.glsl";
    auto* material = m_Materials.Register(materialGuid, doc);
    ASSERT_NE(material, nullptr);
    const StringId slot = HashStringId("albedoMap");
    m_Textures.TrackMaterialTextureRefForTesting(materialGuid, slot, texture);
    Access::Queue(m_Textures, texture, materialGuid, asset, slot);
    m_Textures.FlushPendingUploads();

    const auto tail = Access::StreamedHandle(m_Textures, texture);
    ASSERT_TRUE(tail.IsValid());
    ASSERT_EQ(material->GetTexture(slot), tail);
    ASSERT_NO_FATAL_FAILURE(ReplaceFileOf(*asset));
    EXPECT_FALSE(m_Textures.GetOrRequestTexture(texture).IsValid());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (Access::InFlight(m_Textures, texture) && std::chrono::steady_clock::now() < deadline)
    {
        EngineCore::GetInstance().GetAssetManager().Update();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_FALSE(Access::InFlight(m_Textures, texture));
    ASSERT_TRUE(Access::Uploaded(m_Textures, texture));
    m_Textures.FlushPendingUploads();
    EXPECT_EQ(Access::StreamedHandle(m_Textures, texture), tail);
    EXPECT_EQ(material->GetTexture(slot), tail);
}

TEST_F(TextureCoverageGPU, AsyncPreloadPollNeverUploadsInlineOrDuplicatesDecodedWork)
{
    using Access = Engine::Renderer::TextureCoverageUploadTestAccess;
    const auto path = Image();
    const GUID guid = Register(path, false);
    ASSERT_FALSE(m_Textures.GetOrRequestTexture(guid).IsValid());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (Access::Queued(m_Textures, guid) == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ASSERT_EQ(Access::Queued(m_Textures, guid), 1u);
    ASSERT_TRUE(Access::InFlight(m_Textures, guid));
    for (int i = 0; i < 128; ++i)
        ASSERT_FALSE(m_Textures.GetOrRequestTexture(guid).IsValid());
    EXPECT_EQ(Access::Queued(m_Textures, guid), 1u);
    EXPECT_EQ(m_Textures.PendingMaterialTextureBindCount(), 0u);
    TextureHandle resident;
    while (!(resident = m_Textures.GetOrRequestTexture(guid)).IsValid() &&
           std::chrono::steady_clock::now() < deadline)
    {
        m_Textures.FlushPendingUploads();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_TRUE(resident.IsValid());
    EXPECT_FALSE(Access::InFlight(m_Textures, guid));
    EXPECT_FALSE(Access::HasLoadHandle(m_Textures, guid));
    EXPECT_EQ(Access::Queued(m_Textures, guid), 0u);
    EXPECT_EQ(m_Textures.GetOrRequestTexture(guid), resident);
    EXPECT_EQ(Render(resident, 64, 64, 0).size(), 64u * 64u * 4u);
}

TEST_F(TextureCoverageGPU, AsyncPreloadEvictionDiscardsQueuedBytesAndAllowsReplacement)
{
    using Access = Engine::Renderer::TextureCoverageUploadTestAccess;
    const auto path = Image();
    const GUID guid = Register(path, false);
    m_Textures.GetOrRequestTexture(guid);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (Access::Queued(m_Textures, guid) == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ASSERT_EQ(Access::Queued(m_Textures, guid), 1u);
    m_Textures.Evict(guid);
    EXPECT_FALSE(Access::InFlight(m_Textures, guid));
    EXPECT_EQ(Access::Queued(m_Textures, guid), 0u);
    EXPECT_FALSE(m_Textures.GetOrRequestTexture(guid).IsValid());
    TextureHandle resident;
    while (!(resident = m_Textures.GetOrRequestTexture(guid)).IsValid() &&
           std::chrono::steady_clock::now() < deadline)
    {
        m_Textures.FlushPendingUploads();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_TRUE(resident.IsValid());
    EXPECT_FALSE(Access::InFlight(m_Textures, guid));
    EXPECT_EQ(Access::Queued(m_Textures, guid), 0u);
}

TEST_F(TextureCoverageGPU, AsyncPreloadBackPressureRetriesAfterCapacityIsReleased)
{
    using Access = Engine::Renderer::TextureCoverageUploadTestAccess;
    std::array<GUID, 32> held;
    for (auto& guid : held)
    {
        guid = GUID::Generate();
        Access::HoldAdmission(m_Textures, guid);
    }
    const auto path = Image();
    const auto requested = Register(path, false);
    for (int i = 0; i < 128; ++i)
        EXPECT_FALSE(m_Textures.GetOrRequestTexture(requested).IsValid());
    EXPECT_FALSE(Access::InFlight(m_Textures, requested));
    EXPECT_EQ(Access::Queued(m_Textures, requested), 0u);
    m_Textures.Evict(held.front());
    EXPECT_FALSE(m_Textures.GetOrRequestTexture(requested).IsValid());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (Access::Queued(m_Textures, requested) == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ASSERT_EQ(Access::Queued(m_Textures, requested), 1u);
    EXPECT_TRUE(Access::InFlight(m_Textures, requested));
    m_Textures.CancelPendingLoads();
    EXPECT_FALSE(Access::InFlight(m_Textures, requested));
    EXPECT_EQ(Access::Queued(m_Textures, requested), 0u);
}

TEST_F(TextureCoverageGPU, AsyncPreloadMissingAssetRetiresRequestWithoutRetryStorm)
{
    using Access = Engine::Renderer::TextureCoverageUploadTestAccess;
    const auto missing = GUID::Generate();
    for (int i = 0; i < 128; ++i)
        EXPECT_FALSE(m_Textures.GetOrRequestTexture(missing).IsValid());
    EXPECT_FALSE(Access::InFlight(m_Textures, missing));
    EXPECT_FALSE(Access::HasLoadHandle(m_Textures, missing));
    EXPECT_EQ(Access::Queued(m_Textures, missing), 0u);
    EXPECT_EQ(m_Textures.PendingMaterialTextureBindCount(), 0u);
}

// Assigning a texture that is not loaded (the editor's texture assign, UpdateMaterialTextures)
// never decodes it on the calling thread: the call returns with the binding pending, the
// AssetManager load cooks the texture on a worker (a cold cache misses), and FlushPendingUploads
// submits and binds the cooked artifact.
TEST_F(TextureCoverageGPU, AssigningATextureThatIsNotLoadedCooksItOnAWorkerAndBindsIt)
{
    const fs::path path = Image();
    const GUID texture = Register(path, false);
    MaterialDocument doc{};
    doc.schemaVersion = 3;
    doc.materialName = "TextureAssign";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "Surfaces/standard_pbr.glsl";
    const GUID material = GUID::Generate();
    ASSERT_NE(m_Materials.Register(material, doc), nullptr);

    doc.textures["albedoMap"] = texture.ToString();
    m_Textures.UpdateMaterialTextures(material, doc);
    EXPECT_FALSE(m_Textures.IsMaterialTextureBindingComplete(material))
        << "the assign loaded and uploaded the texture on the calling thread";

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!m_Textures.IsMaterialTextureBindingComplete(material) && std::chrono::steady_clock::now() < deadline)
    {
        EngineCore::GetInstance().GetAssetManager().Update();
        m_Textures.FlushPendingUploads();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_TRUE(m_Textures.IsMaterialTextureBindingComplete(material)) << "the worker load never bound";
    const Engine::Renderer::Material* bound = m_Materials.Find(material);
    ASSERT_NE(bound, nullptr);
    EXPECT_TRUE(bound->GetTexture(HashStringId("albedoMap")).IsValid());
    EXPECT_EQ(CookedArtifacts(texture), 1u) << "the assign uploaded a raw decode instead of cooking";
}

TEST_F(TextureCoverageGPU, AssigningATextureThatIsAlreadyLoadedBindsIt)
{
    const fs::path path = Image();
    const GUID texture = Register(path, false);
    auto& assets = EngineCore::GetInstance().GetAssetManager();
    auto loaded = std::make_shared<std::atomic<bool>>(false);
    AssetLoadHandle handle =
        assets.LoadAsset(texture, [loaded](Result<SharedPtr<Asset>, AssetError>) { loaded->store(true); });
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!loaded->load() && std::chrono::steady_clock::now() < deadline)
    {
        assets.Update();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_NE(assets.GetAsset(texture), nullptr) << "the texture did not load";

    MaterialDocument doc{};
    doc.schemaVersion = 3;
    doc.materialName = "TextureAssignLoaded";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "Surfaces/standard_pbr.glsl";
    const GUID material = GUID::Generate();
    ASSERT_NE(m_Materials.Register(material, doc), nullptr);
    doc.textures["albedoMap"] = texture.ToString();
    m_Textures.UpdateMaterialTextures(material, doc);

    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!m_Textures.IsMaterialTextureBindingComplete(material) && std::chrono::steady_clock::now() < deadline)
    {
        assets.Update();
        m_Textures.FlushPendingUploads();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_TRUE(m_Textures.IsMaterialTextureBindingComplete(material)) << "the loaded texture never bound";
    const Engine::Renderer::Material* bound = m_Materials.Find(material);
    ASSERT_NE(bound, nullptr);
    const TextureHandle albedo = bound->GetTexture(HashStringId("albedoMap"));
    ASSERT_TRUE(albedo.IsValid());
    EXPECT_EQ(WidthOf(albedo), 64u);
}

// GetOrUpload runs on the render thread and on extraction workers: a cook source it has not
// uploaded yet comes back as no texture at once, and lands through the drain. Nothing about it
// is decoded or mipped on the calling thread (an upload log line names that thread).
TEST_F(TextureCoverageGPU, GetOrUploadNeverPreparesACookSourceOnTheCallingThread)
{
    std::vector<std::string> lines;
    TestLog::ScopedEngineLogCapture capture(&lines);
    const fs::path path = Image();
    const GUID texture = Register(path, false);
    EXPECT_FALSE(m_Textures.GetOrUpload(texture).IsValid()) << "the texture was loaded on the calling thread";
    EXPECT_TRUE(WaitForUpload(texture).IsValid()) << "the texture never uploaded";
    Logger::Log::Flush();
    const std::string name = path.filename().string();
    const auto prepared = std::find_if(lines.begin(), lines.end(), [&](const std::string& line)
                                       { return line.find(name) != std::string::npos &&
                                                line.find("on the render thread") != std::string::npos; });
    EXPECT_EQ(prepared, lines.end()) << *prepared;
}

// A load result whose epoch an Evict has since moved is never published, whatever route queued it.
TEST_F(TextureCoverageGPU, ALoadResultFromASupersededEpochIsNotPublished)
{
    using Access = Engine::Renderer::TextureCoverageUploadTestAccess;
    const auto path = Image();
    const GUID texture = Register(path, false);
    auto decoded = MakeShared<TextureAsset>(texture, path);
    ASSERT_TRUE(decoded->Load());
    Access::Queue(m_Textures, texture, GUID::Generate(), decoded);
    Access::SupersedeLoadEpoch(m_Textures, texture);
    m_Textures.FlushPendingUploads();
    EXPECT_FALSE(Access::Uploaded(m_Textures, texture)) << "a superseded load was published";
}

// Evicting a texture while its upload job runs drops the job's result: it is a pre-eviction
// load, so it is never published, and the slot binds the texture assigned after it. The evicted
// texture is large enough that its decode and mips are still running when Evict lands.
TEST_F(TextureCoverageGPU, EvictingATextureWhileItsUploadJobRunsDropsTheJobResult)
{
    using Access = Engine::Renderer::TextureCoverageUploadTestAccess;
    const StringId slot = HashStringId("albedoMap");
    MaterialDocument doc{};
    doc.schemaVersion = 3;
    doc.materialName = "TextureEvictWhileLoading";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "Surfaces/standard_pbr.glsl";
    const GUID material = GUID::Generate();
    ASSERT_NE(m_Materials.Register(material, doc), nullptr);
    const Engine::Renderer::Material* bound = m_Materials.Find(material);
    ASSERT_NE(bound, nullptr);

    for (int round = 0; round < 8; ++round)
    {
        SCOPED_TRACE(round);
        const GUID evicted = Register(Image(false, 2048), false);
        const GUID kept = Register(Image(), false);
        doc.textures["albedoMap"] = evicted.ToString();
        m_Textures.UpdateMaterialTextures(material, doc);
        doc.textures["albedoMap"] = kept.ToString();
        m_Textures.UpdateMaterialTextures(material, doc);
        m_Textures.Evict(evicted);

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while ((!m_Textures.IsMaterialTextureBindingComplete(material) || !Access::UploadJobsIdle(m_Textures)) &&
               std::chrono::steady_clock::now() < deadline)
        {
            m_Textures.FlushPendingUploads();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        m_Textures.FlushPendingUploads();
        ASSERT_TRUE(Access::UploadJobsIdle(m_Textures)) << "an upload job never finished";
        EXPECT_FALSE(Access::Uploaded(m_Textures, evicted)) << "the evicted texture's job result was published";
        EXPECT_EQ(bound->GetTexture(slot), m_Textures.GetOrUpload(kept));
    }
}

// Shutdown with upload jobs in flight waits for them, so none outlives the service.
TEST_F(TextureCoverageGPU, ShutdownWaitsForUploadJobsStillRunning)
{
    using Access = Engine::Renderer::TextureCoverageUploadTestAccess;
    MaterialDocument doc{};
    doc.schemaVersion = 3;
    doc.materialName = "TextureShutdownWhileLoading";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "Surfaces/standard_pbr.glsl";
    const GUID material = GUID::Generate();
    ASSERT_NE(m_Materials.Register(material, doc), nullptr);
    // Large enough that the jobs are still decoding when Shutdown runs.
    for (int i = 0; i < 4; ++i)
    {
        doc.textures["albedoMap"] = Register(Image(false, 2048), false).ToString();
        m_Textures.UpdateMaterialTextures(material, doc);
    }
    m_Textures.Shutdown();
    EXPECT_TRUE(Access::UploadJobsIdle(m_Textures)) << "Shutdown returned with an upload job still running";
}

// A second assign to a slot supersedes a first whose load has not landed yet: the first texture's
// upload (here a landed job result queued for the drain) must not bind over the second.
TEST_F(TextureCoverageGPU, ALaterAssignSupersedesAnEarlierOneStillLoading)
{
    using Access = Engine::Renderer::TextureCoverageUploadTestAccess;
    const StringId slot = HashStringId("albedoMap");
    const auto firstPath = Image();
    const GUID first = Register(firstPath, false);
    const GUID second = Register(Image(), false);
    MaterialDocument doc{};
    doc.schemaVersion = 3;
    doc.materialName = "TextureReassign";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "Surfaces/standard_pbr.glsl";
    const GUID material = GUID::Generate();
    ASSERT_NE(m_Materials.Register(material, doc), nullptr);

    // The first assign's load has landed but is not uploaded yet.
    auto firstDecoded = MakeShared<TextureAsset>(first, firstPath);
    ASSERT_TRUE(firstDecoded->Load());
    Access::Queue(m_Textures, first, material, firstDecoded, slot);
    doc.textures["albedoMap"] = second.ToString();
    m_Textures.UpdateMaterialTextures(material, doc);
    m_Textures.FlushPendingUploads();
    const Engine::Renderer::Material* bound = m_Materials.Find(material);
    ASSERT_NE(bound, nullptr);
    const Rendering::TextureHandle firstUpload = m_Textures.GetOrUpload(first);
    ASSERT_TRUE(firstUpload.IsValid());
    EXPECT_NE(bound->GetTexture(slot), firstUpload) << "the superseded assign bound when its load landed";

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!m_Textures.IsMaterialTextureBindingComplete(material) && std::chrono::steady_clock::now() < deadline)
    {
        EngineCore::GetInstance().GetAssetManager().Update();
        m_Textures.FlushPendingUploads();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_TRUE(m_Textures.IsMaterialTextureBindingComplete(material));
    EXPECT_EQ(bound->GetTexture(slot), m_Textures.GetOrUpload(second));
}

TEST_F(TextureCoverageGPU, LateInvalidPolicyDrainsQueuedBindAndLoadHandleThenRecovers)
{
    using Access = Engine::Renderer::TextureCoverageUploadTestAccess;
    const auto path = Image(true);
    const GUID guid = Register(path, true);
    auto decoded = MakeShared<TextureAsset>(guid, path);
    ASSERT_TRUE(decoded->Load());
    ASSERT_EQ(decoded->GetChannels(), 2u);
    const auto material = GUID::Generate();
    Access::Queue(m_Textures, guid, material, decoded);
    ASSERT_EQ(m_Textures.PendingMaterialTextureBindCount(), 1u);
    ASSERT_TRUE(Access::HasLoadHandle(m_Textures, guid));
    // Metadata changed after successful decode but before the render-thread drain.
    Set(path, kTextureAlphaCutoffMetaKey, "nan");
    m_Textures.FlushPendingUploads();
    EXPECT_EQ(m_Textures.PendingMaterialTextureBindCount(), 0u);
    EXPECT_FALSE(Access::HasLoadHandle(m_Textures, guid));
    EXPECT_TRUE(m_Textures.IsMaterialTextureBindingComplete(material));
    EXPECT_FALSE(m_Textures.GetOrUpload(guid).IsValid());
    Set(path, kTextureAlphaCutoffMetaKey, ".538");
    m_Textures.Evict(guid);
    Access::Queue(m_Textures, guid, material, decoded);
    m_Textures.FlushPendingUploads();
    // CPU mip preparation is now asynchronous. Pump owner-thread GPU work;
    // don't trigger GetOrUpload's synchronous fallback to hide a broken queue.
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while (m_Textures.PendingMaterialTextureBindCount()!=0&&std::chrono::steady_clock::now()<deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        m_Textures.FlushPendingUploads();
    }
    EXPECT_EQ(m_Textures.PendingMaterialTextureBindCount(), 0u);
    EXPECT_FALSE(Access::HasLoadHandle(m_Textures, guid));
    const auto recovered = m_Textures.GetOrUpload(guid);
    ASSERT_TRUE(recovered.IsValid());
    const auto pixels = Render(recovered, 64, 64, 0);
    ASSERT_EQ(pixels.size(), 64u * 64u * 4u);
    bool transparent = false, opaque = false;
    for (size_t i = 0; i < pixels.size(); i += 4)
    {
        EXPECT_FLOAT_EQ(pixels[i], pixels[i + 1]);
        EXPECT_FLOAT_EQ(pixels[i], pixels[i + 2]);
        transparent |= pixels[i + 3] == 0;
        opaque |= pixels[i + 3] == 1;
    }
    EXPECT_TRUE(transparent);
    EXPECT_TRUE(opaque);
}

TEST_F(TextureCoverageGPU, InvalidMetadataCannotPublishUncorrectedRawTexture)
{
    for (const auto& invalid : {std::array<const char*, 3>{"true", ".538", "none"},
                                {"1", "nan", "none"},
                                {"1", ".538", "bc1"},
                                {"1", ".538", "bc4"},
                                {"1", ".538", "bc5"},
                                {"1", ".538", "bc6h"}})
    {
        const auto path = Image();
        const GUID guid = Register(path, true);
        Set(path, kTextureAlphaCoverageMetaKey, invalid[0]);
        Set(path, kTextureAlphaCutoffMetaKey, invalid[1]);
        Set(path, kTextureCompressionMetaKey, invalid[2]);
        EXPECT_FALSE(WaitForUpload(guid).IsValid());
        // The refusal is cached as no texture, so GetOrUpload does not request it every frame.
        EXPECT_TRUE(Engine::Renderer::TextureCoverageUploadTestAccess::Uploaded(m_Textures, guid))
            << "a texture that cannot load was not cached";
        Set(path, kTextureAlphaCoverageMetaKey, "0");
        Set(path, kTextureCompressionMetaKey, "none");
        m_Textures.Evict(guid);
        EXPECT_TRUE(WaitForUpload(guid).IsValid());
    }
}

// A far leaf reads the last levels of an ordinary mip chain, where the filtered alpha of a 0/1
// mask is below the cutoff. The foliage coverage test keeps the base coverage at the levels of 4
// texels across and fewer, down to 1 x 1, where the cutoff test keeps no pixel.
TEST_F(TextureCoverageGPU, FoliageCoverageKeepsTheBaseCoverageThroughTheMipTail)
{
    const GUID guid = Register(Image(), false);
    const auto texture = WaitForUpload(guid);
    ASSERT_TRUE(texture.IsValid());
    constexpr uint32 kCutoffMode = 1, kFoliageMode = 2;
    constexpr float kFourTexelLod = 4, kTailLod = 6; // 4 x 4 and 1 x 1 for the 64 x 64 image
    const double reference = Coverage(Render(texture, 256, 256, 0, kCutoffMode));
    ASSERT_GT(reference, 0.2);
    EXPECT_EQ(Coverage(Render(texture, 256, 256, 0, kFoliageMode)), reference);
    for (float lod = kFourTexelLod; lod <= kTailLod; lod += .5f)
        EXPECT_NEAR(Coverage(Render(texture, 256, 256, lod, kFoliageMode)), reference, 0.03) << "lod " << lod;
    EXPECT_EQ(Coverage(Render(texture, 256, 256, kTailLod, kCutoffMode)), 0.0);
}

// Two far leaves in one pixel with the same seed pass together and cover only c. A seed turns
// the threshold, so seeds 0 and 0.5 pass on disjoint thresholds and cover 2c (c < 0.5). Over
// random seeds the expected cover is 1 - (1 - c)^2, as for separate leaves.
TEST_F(TextureCoverageGPU, FoliageCoverageOfOverlappingLeavesAddsUpOnlyWithDifferentSeeds)
{
    const GUID guid = Register(Image(), false);
    const auto texture = WaitForUpload(guid);
    ASSERT_TRUE(texture.IsValid());
    constexpr uint32 kFoliageMode = 2, kTwoLeafMode = 3;
    constexpr float kTailLod = 6;
    const double single = Coverage(Render(texture, 256, 256, kTailLod, kFoliageMode));
    ASSERT_GT(single, 0.2);
    ASSERT_LT(single, 0.5);
    EXPECT_NEAR(Coverage(Render(texture, 256, 256, kTailLod, kTwoLeafMode, 0.0f, 0.0f)), single, 0.01);
    EXPECT_NEAR(Coverage(Render(texture, 256, 256, kTailLod, kTwoLeafMode, 0.0f, 0.5f)), 2.0 * single, 0.03);
}

TEST_F(TextureCoverageGPU, FullChainMaskedCardsMeasureIntegerFractionalAndPhaseCoverage)
{
#if defined(GE_HAVE_KTX)
    const auto compressionCases = {false, true};
#else
    const auto compressionCases = {false};
#endif
    for (bool bc7 : compressionCases)
    {
        SCOPED_TRACE(bc7);
        const auto ordinaryPath = Image(), coveredPath = Image();
        const GUID ordinaryGuid = Register(ordinaryPath, false), coveredGuid = Register(coveredPath, true);
        if (bc7)
        {
            if (!IsTextureCookEncoderAvailable() ||
                TextureAsset::GetGpuTranscodeTarget() != TextureGpuTranscodeTarget::BC7)
                GTEST_SKIP() << "BC7 encoder or device sampling unavailable";
            for (const auto& pair : {std::pair{ordinaryPath, ordinaryGuid}, std::pair{coveredPath, coveredGuid}})
            {
                Set(pair.first, kTextureCompressionMetaKey, "bc7");
                AssetManager::ScopedThreadAssetManager context(&EngineCore::GetInstance().GetAssetManager());
                TextureAsset cooked(pair.second, pair.first);
                ASSERT_TRUE(cooked.Load());
                ASSERT_EQ(cooked.GetFormat(), GameEngine::TextureFormat::BC7);
                ASSERT_EQ(cooked.GetMipChain().size(), 7u);
            }
        }
        const auto ordinary = WaitForUpload(ordinaryGuid);
        const auto covered = WaitForUpload(coveredGuid);
        ASSERT_TRUE(ordinary.IsValid());
        ASSERT_TRUE(covered.IsValid());
        const double reference = Coverage(Render(ordinary, 256, 256, 0, 1));
        double ordinaryError = 0, coveredError = 0;
        for (float lod = 0; lod <= 6; lod += .5f)
            for (float phase : {0.0f, .31f / 64.0f, .73f / 64.0f})
            {
                const double before = Coverage(Render(ordinary, 256, 256, lod, 1, phase));
                const double after = Coverage(Render(covered, 256, 256, lod, 1, phase));
                EXPECT_GE(after, 0.0);
                EXPECT_LE(after, 1.0);
                // Coarsest tied levels are measured, but cannot represent the base ratio.
                // Improvement is asserted only over this complete, explicitly fixed sample set.
                ordinaryError += std::abs(before - reference);
                coveredError += std::abs(after - reference);
                std::cout << "coverage-sample bc7=" << bc7 << " lod=" << lod << " phase=" << phase << " base=" << reference
                          << " ordinary=" << before << " preserved=" << after << '\n';
            }
        EXPECT_LT(coveredError, ordinaryError);
        if (bc7)
        {
            AssetManager::ScopedThreadAssetManager context(&EngineCore::GetInstance().GetAssetManager());
            TextureAsset cooked(coveredGuid, coveredPath);
            ASSERT_TRUE(cooked.Load());
            for (uint32 level = 0; level < 7; ++level)
            {
                const auto& mip = cooked.GetMipChain()[level];
                std::vector<uint8> decoded;
                ASSERT_TRUE(DecodeBlockPayloadRGBA8(cooked.GetFormat(), mip.Width, mip.Height,
                                                    cooked.GetPixelData() + mip.Offset, mip.Size, decoded));
                const auto gpu = Render(covered, mip.Width, mip.Height, float(level));
                ASSERT_EQ(decoded.size(), gpu.size());
                for (size_t i = 0; i < decoded.size(); ++i)
                {
                    const float expected = i % 4 == 3 ? decoded[i] / 255.0f : LinearByte(decoded[i]);
                    // Independent BC7 decoders may round by one UNORM8 code;
                    // the RGB allowance also includes the sRGB transfer slope.
                    EXPECT_NEAR(gpu[i], expected, i % 4 == 3 ? 1.0f / 255.0f : 0.012f);
                }
            }
        }
    }
}
