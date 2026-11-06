#include "ShaderGraph/GraphPreviewAtlas.h"

#include "Engine/Rendering/IEnvironmentSource.h"
#include "Engine/Rendering/ImageBasedLightingFeature.h"
#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/MaterialSystem.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "Engine/Rendering/PrimitiveGenerator.h"
#include "Engine/Rendering/PipelineVariantCache.h"
#include "Engine/Rendering/RenderServices.h"
#include "Rendering/Common/Utils.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/BarrierMapping.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Materials/MaterialDocument.h"
#include "Rendering/Materials/ShaderCompileService.h"
#include "Rendering/Materials/ShaderLayoutShape.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"
#include "StagedTestPaths.h"
#include "TestTempDir.h"
#include "Types/Color.h"
#include "UIRgTestHarness.h"
#include "UI/Parsers/XMLParser.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/UIManager.h"

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

using namespace GameEngine;
using namespace GameEngine::Editor;
using namespace GameEngine::Engine::Renderer;
using namespace GameEngine::Rendering;
namespace RG = GameEngine::Rendering::RenderGraph;

namespace
{
constexpr uint64_t kWindow = 11u;

std::filesystem::path ResolveStagedShaderPath(const std::filesystem::path& relativePath)
{
    const auto staged = TestPaths::StagedRoot() / relativePath;
    std::error_code error;
    return std::filesystem::is_regular_file(staged, error) ? staged : std::filesystem::path{};
}

class ScopedStagedShaderHooks
{
  public:
    ScopedStagedShaderHooks()
        : m_PreviousLoader(Rendering::Utils::GetShaderFileLoader()),
          m_PreviousResolver(Rendering::Utils::GetShaderPathResolver())
    {
        Rendering::Utils::SetShaderFileLoader(nullptr);
        Rendering::Utils::SetShaderPathResolver(&ResolveStagedShaderPath);
    }
    ~ScopedStagedShaderHooks()
    {
        Rendering::Utils::SetShaderFileLoader(m_PreviousLoader);
        Rendering::Utils::SetShaderPathResolver(m_PreviousResolver);
    }
    ScopedStagedShaderHooks(const ScopedStagedShaderHooks&) = delete;
    ScopedStagedShaderHooks& operator=(const ScopedStagedShaderHooks&) = delete;

  private:
    Rendering::Utils::ShaderFileLoaderFunc m_PreviousLoader;
    Rendering::Utils::ShaderPathResolverFunc m_PreviousResolver;
};

// Every opened device frame is finalized before its resources leave scope.
class DeviceFrame
{
  public:
    explicit DeviceFrame(IDevice& device) : m_Device(device) {}
    bool Open()
    {
        m_Open = m_Device.BeginFrame();
        return m_Open;
    }
    ~DeviceFrame()
    {
        if (m_Open)
        {
            m_Device.FinalizeFrame();
            m_Device.WaitForIdle();
        }
    }
  private:
    IDevice& m_Device;
    bool m_Open = false;
};

struct DescriptorWitness
{
    uint32_t Before = 0, After = 0;
    bool Observed = false;
};

class GraphPreviewAtlasPreparationTest : public ::testing::TestWithParam<GraphicsAPI>
{
  protected:
    void SetUp() override
    {
        DeviceDesc description{};
        description.preferredAPI = GetParam();
        description.enableSwapchain = false;
        description.enableDebugLayer = false;
        description.pipelineCacheDirectory = (m_Cache.Path() / "PipelineCache").string();
        description.descriptorBuffers = DescriptorBufferMode::Disabled;
        m_Device = DeviceFactory::CreateDevice(description);
        if (!m_Device || !m_Device->Initialize(description))
            GTEST_SKIP() << "requested atlas native backend unavailable";
        ASSERT_EQ(m_Device->GetAPI(), GetParam());
        m_Services = std::make_unique<RenderServices>();
        ASSERT_TRUE(m_Services->Initialize(m_Device.get()));
        m_Services->SetDefaultMSAASampleCount(1u);
        const auto shaderDirectory = TestPaths::StagedRenderingShadersDir();
        ASSERT_TRUE(std::filesystem::is_directory(shaderDirectory));
        MaterialBuildContext context{};
        context.AdapterShaderDir = shaderDirectory;
        context.CacheRoot = m_Cache.Path() / "Materials";
        context.IncludeDirs = {shaderDirectory};
        m_Services->Materials().SetMaterialBuildContext(context);
        PrimitiveGenerator::RegisterAll(*m_Services);
        const auto* sphere = m_Services->GetMeshGPURegistry().FindByKey({PrimitiveGenerator::SphereGuid(), 0});
        ASSERT_NE(sphere, nullptr);
        MeshGPUEntryBindings bindings{};
        ASSERT_TRUE(m_Services->GetMeshGPURegistry().TryGetDrawableBindings(*sphere, bindings));
        ASSERT_NO_FATAL_FAILURE(RegisterColor(m_Red, {1, 0, 0, 1}));
        ASSERT_NO_FATAL_FAILURE(RegisterColor(m_Green, {0, 1, 0, 1}));
        ASSERT_NO_FATAL_FAILURE(RegisterColor(m_Blue, {0, 0, 1, 1}));
        ASSERT_NO_FATAL_FAILURE(CreateProbe());
    }

    void TearDown() override
    {
        if (!m_Device)
            return;
        m_Device->WaitForIdle();
        if (m_Readback.IsValid()) m_Device->DestroyBuffer(m_Readback);
        if (m_ProbeOutput.IsValid()) m_Device->DestroyTexture(m_ProbeOutput);
        if (m_Sampler.IsValid()) m_Device->DestroySampler(m_Sampler);
        if (m_Services) m_Services->Shutdown();
        m_Services.reset();
        m_Device->Shutdown();
    }

    void RegisterColor(const GUID& guid, const ColorLinear& color)
    {
        MaterialDocument document{};
        document.materialName = "Atlas native color";
        document.lightingModel = "Unlit";
        document.surfaceShader = "Surfaces/unlit_solid.glsl";
        document.properties["baseColor"] = std::vector<float>{color.r, color.g, color.b, color.a};
        const auto* material = m_Services->Materials().RegisterMaterialFromDocument(guid, document);
        ASSERT_NE(material, nullptr);
        ASSERT_TRUE(material->GetGraphicsPipelineId().IsValid());
    }

    void CompileIblVariant(Material& material, VertexAttributeFlags flags,
                           PipelineVariantCache::VariantEntrySnapshotForTesting& published,
                           std::optional<FrontFace> winding = std::nullopt)
    {
        GraphicsPipelineDesc basePipeline{};
        ASSERT_TRUE(m_Device->CopyGraphicsPipeline(material.GetGraphicsPipelineId(), basePipeline));
        auto& variants = m_Services->Materials().Variants();
        const auto key = variants.EnqueueColorVariantCompileForTesting(
            material, flags, PrimitiveTopology::TriangleList, MaterialKeyword::IBL,
            1u, winding.value_or(basePipeline.Rasterization.frontFace));
        variants.SubmitPendingVariantCompiles();
        m_Services->Materials().FlushAsyncMaterialCompiles();
        published = variants.SnapshotColorVariantForTesting(key);
        ASSERT_TRUE(published.CurrentPipelineId.IsValid());
        ASSERT_EQ(published.MaterialVersion, material.GetVersion());
    }

    void CreateProbe()
    {
        ShaderProgramCompileRequest request{};
        request.debugName = "AtlasNativeSample";
        request.baseDirectory = m_Cache.Path();
        request.cacheRoot = m_Cache.Path() / "Probe";
        ShaderStageCompileSpec stage{};
        stage.stage = "cs";
        stage.sourcePath = m_Cache.Path() / "probe.comp";
        stage.inlineSource = R"glsl(#version 450
layout(local_size_x=1, local_size_y=1, local_size_z=1) in;
layout(rgba32f,set=0,binding=0) writeonly uniform image2D result;
layout(set=0,binding=1) uniform sampler2D atlas;
void main() { imageStore(result, ivec2(0), texelFetch(atlas, ivec2(128,128), 0)); }
)glsl";
        request.stages.push_back(stage);
        ShaderProgramCompileResult result{};
        std::string error;
        ASSERT_TRUE(ShaderCompileService::CompileProgramToCache(
            request, ShaderSourceKind::SpirV, result, &error)) << error;
        for (const auto& [binding, type] : {
                 std::pair{0u, DescriptorType::StorageImage},
                 std::pair{1u, DescriptorType::CombinedImageSampler}})
        {
            DescriptorBinding descriptor{};
            descriptor.binding = binding;
            descriptor.type = type;
            descriptor.count = 1;
            descriptor.shaderStages = kShaderStageCompute;
            m_ProbeLayout.bindings.push_back(descriptor);
        }
        ApplyMetaImageShapeToLayout(result.meta, m_ProbeLayout);
        ComputePipelineDesc description{};
        description.ComputeShader = std::make_shared<const std::vector<uint8_t>>(result.stageBytes.at("cs"));
        description.DescriptorSetLayouts.push_back(m_Device->InternDescriptorSetLayout(m_ProbeLayout));
        m_Probe = m_Device->GetOrCreateComputePipeline(m_Device->InternComputePipeline(description));
        ASSERT_TRUE(m_Probe.IsValid());
        TextureDesc output{};
        output.width = output.height = output.depth = 1u;
        output.mipLevels = output.arrayLayers = output.sampleCount = 1u;
        output.format = static_cast<uint32_t>(TextureFormat::R32G32B32A32_FLOAT);
        output.usage = static_cast<uint32_t>(TextureUsage::UnorderedAccess | TextureUsage::TransferSrc);
        output.initialState = ResourceState::UnorderedAccess;
        output.debugName = "Atlas native probe output";
        m_ProbeOutput = m_Device->CreateTexture(output);
        m_Readback = m_Device->CreateReadbackBuffer(sizeof(float) * 4u, "Atlas native probe readback");
        m_Sampler = m_Device->CreateSampler(SamplerDesc::MaterialLinearClamp("Atlas native probe"));
        ASSERT_TRUE(m_ProbeOutput.IsValid() && m_Readback.IsValid() && m_Sampler.IsValid());
    }

    // Four contiguous 32-bit channels match the RGBA32F readback buffer.
    void Sample(TextureHandle texture, std::array<float, 4>& value)
    {
        ASSERT_TRUE(texture.IsValid());
        DescriptorSetDesc description{};
        description.layout = m_ProbeLayout;
        const auto set = m_Device->CreateDescriptorSet(description);
        ASSERT_TRUE(set.IsValid());
        m_Device->UpdateStorageImageBinding(set, 0, m_ProbeOutput);
        m_Device->UpdateCombinedImageSamplerBinding(set, 1, texture, m_Sampler);
        auto commands = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        ASSERT_NE(commands, nullptr);
        commands->Begin();
        // The graph exports fragment sampling. The probe also needs compute visibility.
        commands->Barrier(ResourceBarrier::CreateMemoryBarrier(
            static_cast<uint64_t>(PipelineStageMask::GraphicsColor | PipelineStageMask::GraphicsFragment),
            static_cast<uint64_t>(PipelineStageMask::ComputeShader),
            static_cast<uint64_t>(ResourceAccessMask::ColorAttachmentWrite | ResourceAccessMask::ShaderRead),
            static_cast<uint64_t>(ResourceAccessMask::ShaderRead)));
        commands->SetPipeline(m_Probe);
        commands->BindDescriptorSet(0, set, m_Probe);
        commands->Dispatch(1, 1, 1);
        commands->Barrier(ResourceBarrier::CreateTextureBarrier(
            m_ProbeOutput, ResourceState::UnorderedAccess, ResourceState::CopySource));
        // Texture readback supplies the transfer-write -> host-read dependency.
        commands->CopyTextureSubresourceToBuffer(m_ProbeOutput, 0u, 0u, m_Readback, 1u, 1u);
        commands->Barrier(ResourceBarrier::CreateTextureBarrier(
            m_ProbeOutput, ResourceState::CopySource, ResourceState::UnorderedAccess));
        commands->End();
        m_Device->ExecuteCommandLists({commands.get()});
        m_Device->WaitForIdle();
        void* mapped = m_Device->MapBuffer(m_Readback);
        ASSERT_NE(mapped, nullptr);
        std::memcpy(value.data(), mapped, sizeof(value));
        m_Device->UnmapBuffer(m_Readback);
    }

    void Build(GraphPreviewAtlas& atlas, RG::RGFrame& frame, uint64_t index, const GUID& material,
               bool* allow = nullptr, DescriptorWitness* witness = nullptr, UIManager* ui = nullptr)
    {
        m_Services->BeginWorldDrawFrame();
        m_Services->GetPerFrameWritePool().BeginFrame(m_Device->GetFrameIndex());
        m_Services->Materials().FinalizeFrameBuffers();
        frame.BeginFrame(index);
        atlas.SetRequests(kWindow, {{"node", material}});
        atlas.TickRG(kWindow, ui, frame);
        GraphNodePreviewBinding binding{};
        ASSERT_TRUE(atlas.TryGetBinding("node", binding));
        const auto suffix = binding.Resource.substr(binding.Resource.find_last_of('_') + 1u);
        const std::string name = "Editor.GraphPreviewAtlas." + suffix;
        RG::RGPassId draw = RG::kInvalidId;
        for (RG::RGPassId pass = 0; pass < frame.Graph().PassCount(); ++pass)
            if (name == frame.Graph().PassName(pass)) draw = pass;
        ASSERT_NE(draw, RG::kInvalidId) << "the fixture must declare a real material draw";
        const std::string tonemapName = name + ".Tonemap";
        bool hasTonemap = false;
        for (RG::RGPassId pass = 0; pass < frame.Graph().PassCount(); ++pass)
            hasTonemap |= tonemapName == frame.Graph().PassName(pass);
        RecordProperty("tonemapPackagePath", ResolveShaderPkgPath("Shaders/tonemap.shaderpkg"));
        ASSERT_TRUE(hasTonemap) << "the fixture must declare the final atlas writer";
        m_LastAtlas = {};
        for (RG::RGResourceId resource = 0; resource < frame.Graph().ResourceCount(); ++resource)
            if (name == frame.Graph().ResourceName(resource)) m_LastAtlas = RG::RGTexture{resource};
        ASSERT_TRUE(m_LastAtlas.IsValid());
        if (witness)
        {
            const auto before = frame.AddPass("Atlas.Before", PassPhase::kEarlySetup,
                [](RG::RGPassBuilder& pass) { pass.PreventCulling(); },
                [this, witness](RG::RGContext&)
                {
                    witness->Before = m_Device->GetDescriptorAllocatorStats().TransientAllocated;
                    witness->Observed = true;
                });
            const auto after = frame.AddPass("Atlas.After", PassPhase::kWorldRender,
                [](RG::RGPassBuilder& pass) { pass.PreventCulling(); },
                [this, witness](RG::RGContext&)
                { witness->After = m_Device->GetDescriptorAllocatorStats().TransientAllocated; });
            frame.AddOrderingEdge(before, RG::RGPass{draw});
            frame.AddOrderingEdge(RG::RGPass{draw}, after);
        }
        if (allow)
            frame.AddPass("Atlas.AllowSubmission", PassPhase::kFinalize,
                [allow](RG::RGPassBuilder& pass)
                {
                    pass.PreventCulling();
                    pass.Prepare([allow](RG::RGPrepareContext&) { return *allow; });
                }, [](RG::RGContext&) {});
    }

    void Execute(RG::RGFrame& frame, bool submit)
    {
        const auto before = frame.SubmittedFrameCount();
        frame.Execute();
        EXPECT_EQ(frame.SubmittedFrameCount(), before + (submit ? 1u : 0u));
    }

    void ExpectColor(TextureHandle texture, size_t channel)
    {
        std::array<float, 4> value{};
        ASSERT_NO_FATAL_FAILURE(Sample(texture, value));
        EXPECT_GT(value[channel], 0.1f);
        EXPECT_GT(value[3], 0.5f) << "the native sphere must occupy the sample";
        for (size_t other = 0; other < 3u; ++other)
            if (other != channel) EXPECT_GT(value[channel], value[other] + 0.1f);
    }

    // Declared first so hooks remain installed through every resource destructor.
    ScopedStagedShaderHooks m_ShaderHooks;
    TestUtils::ScopedTempDir m_Cache{TestUtils::MakeUniqueTempDirectory("atlas-native-preparation")};
    std::unique_ptr<IDevice> m_Device;
    std::unique_ptr<RenderServices> m_Services;
    GUID m_Red = GUID::Generate(), m_Green = GUID::Generate(), m_Blue = GUID::Generate();
    PipelineHandle m_Probe{};
    DescriptorSetLayoutDesc m_ProbeLayout;
    BufferHandle m_Readback{};
    TextureHandle m_ProbeOutput{};
    SamplerHandle m_Sampler{};
    RG::RGTexture m_LastAtlas{};
};

TEST_P(GraphPreviewAtlasPreparationTest, NativePixelsSurviveAbortRetryAndRefuseSupersededGraphs)
{
    GraphPreviewAtlas atlas;
    atlas.Initialize(m_Services.get());
    UiRgHarness firstPools(m_Device.get()), oldPools(m_Device.get()), newerPools(m_Device.get());
    RG::RGFrame first(m_Device.get(), &firstPools.Persistent, &firstPools.Transient, &firstPools.Ring);
    RG::RGFrame old(m_Device.get(), &oldPools.Persistent, &oldPools.Transient, &oldPools.Ring);
    RG::RGFrame newer(m_Device.get(), &newerPools.Persistent, &newerPools.Transient, &newerPools.Ring);
    DescriptorWitness witness;
    TextureHandle output{};
    {
        DeviceFrame active(*m_Device);
        ASSERT_TRUE(active.Open());
        ASSERT_NO_FATAL_FAILURE(Build(atlas, first, 1u, m_Red, nullptr, &witness));
        const auto allocatedAtDeclaration = m_Device->GetDescriptorAllocatorStats().TransientAllocated;
        ASSERT_NO_FATAL_FAILURE(Execute(first, true));
        output = first.PhysicalTexture(m_LastAtlas);
        if (GetParam() == GraphicsAPI::Vulkan)
        {
            EXPECT_TRUE(witness.Observed);
            EXPECT_GT(witness.Before, allocatedAtDeclaration);
            EXPECT_EQ(witness.After, witness.Before) << "atlas replay must not allocate descriptor sets";
        }
    }
    ASSERT_NO_FATAL_FAILURE(ExpectColor(output, 0u));
    bool allow = false;
    {
        DeviceFrame active(*m_Device);
        ASSERT_TRUE(active.Open());
        ASSERT_NO_FATAL_FAILURE(Build(atlas, old, 2u, m_Green, &allow));
        ASSERT_NO_FATAL_FAILURE(Execute(old, false));
        ASSERT_NO_FATAL_FAILURE(Execute(old, false));
    }
    ASSERT_NO_FATAL_FAILURE(ExpectColor(output, 0u));
    allow = true;
    {
        DeviceFrame active(*m_Device);
        ASSERT_TRUE(active.Open());
        ASSERT_NO_FATAL_FAILURE(Execute(old, true));
    }
    ASSERT_NO_FATAL_FAILURE(ExpectColor(output, 1u));
    allow = false;
    {
        DeviceFrame active(*m_Device);
        ASSERT_TRUE(active.Open());
        ASSERT_NO_FATAL_FAILURE(Build(atlas, old, 3u, m_Red, &allow));
        ASSERT_NO_FATAL_FAILURE(Execute(old, false));
    }
    {
        DeviceFrame active(*m_Device);
        ASSERT_TRUE(active.Open());
        ASSERT_NO_FATAL_FAILURE(Build(atlas, newer, 4u, m_Blue));
        ASSERT_NO_FATAL_FAILURE(Execute(newer, true));
        ASSERT_EQ(newer.PhysicalTexture(m_LastAtlas), output);
    }
    ASSERT_NO_FATAL_FAILURE(ExpectColor(output, 2u));
    allow = true;
    {
        DeviceFrame active(*m_Device);
        ASSERT_TRUE(active.Open());
        ASSERT_NO_FATAL_FAILURE(Execute(old, false));
    }
    ASSERT_NO_FATAL_FAILURE(ExpectColor(output, 2u));
}

TEST_P(GraphPreviewAtlasPreparationTest, IblKeepsTheFrontFacingSphereSurface)
{
    const auto surface = m_Cache.Path() / "ibl_fresnel_facing.glsl";
    {
        std::ofstream shader(surface);
        shader << R"glsl(#include "Graph/Nodes/Utility/Nodes.glsl"
SurfaceOutput EvaluateSurface(SurfaceInput sIn)
{
    SurfaceOutput surfaceOutput = DefaultSurfaceOutput();
    float fresnel = SG_Fresnel(sIn.normalWS, sIn.viewDirWS, 2.6);
    surfaceOutput.baseColor = vec3(0.0);
    surfaceOutput.metallic = 1.0;
    surfaceOutput.roughness = 1.0;
    surfaceOutput.ao = 0.0;
    surfaceOutput.normalWS = normalize(sIn.normalWS);
    surfaceOutput.emissive = 2.0 * vec3(fresnel, 1.0 - fresnel, 0.0);
    surfaceOutput.opacity = 1.0;
    return surfaceOutput;
}
)glsl";
    }
    auto& ibl = m_Services->EnsureFeature<ImageBasedLightingFeature>();
    ASSERT_TRUE(ibl.Initialize(m_Device.get()));
    m_Device->WaitForIdle();
    ASSERT_TRUE(ibl.IsInitialized());
    ASSERT_TRUE(ibl.GetIrradianceCube().IsValid() && ibl.GetPrefilterCube().IsValid());
    ASSERT_TRUE(ibl.GetBrdfLut().IsValid());
    MaterialDocument document{};
    document.materialName = "Atlas Fresnel facing";
    document.lightingModel = "StandardPBR";
    document.surfaceShader = surface.string();
    const GUID guid = GUID::Generate();
    auto* material = m_Services->Materials().RegisterMaterialFromDocument(guid, document);
    ASSERT_NE(material, nullptr);
    if (!material->GetGraphicsPipelineId().IsValid())
    {
        for (const auto& compileError : m_Services->Materials().ShaderErrors().Snapshot())
            for (const auto& diagnostic : compileError.Errors)
                ADD_FAILURE() << compileError.MaterialName << ": " << diagnostic;
    }
    ASSERT_TRUE(material->GetGraphicsPipelineId().IsValid());
    const auto* sphere = m_Services->GetMeshGPURegistry().FindByKey({PrimitiveGenerator::SphereGuid(), 0});
    ASSERT_NE(sphere, nullptr);
    PipelineFormatKey format{};
    format.ColorCount = 1u;
    format.ColorFormats[0] = TextureFormat::R16G16B16A16_FLOAT;
    format.DepthFormat = m_Services->GetDepthFormat();
    format.RasterizationSamples = 1u;
    // Both windings are warm so a wrong-face failure must contain real pixels.
    for (const auto winding : {FrontFace::CounterClockwise, FrontFace::Clockwise})
    {
        PipelineVariantCache::VariantEntrySnapshotForTesting published;
        ASSERT_NO_FATAL_FAILURE(CompileIblVariant(*material, sphere->vertexFlags, published, winding));
        ASSERT_TRUE(m_Device->GetOrCreateGraphicsPipeline(published.CurrentPipelineId, format).IsValid());
    }
    GraphPreviewAtlas atlas;
    atlas.Initialize(m_Services.get());
    UiRgHarness basePools(m_Device.get()), iblPools(m_Device.get());
    RG::RGFrame baseFrame(m_Device.get(), &basePools.Persistent, &basePools.Transient, &basePools.Ring);
    RG::RGFrame iblFrame(m_Device.get(), &iblPools.Persistent, &iblPools.Transient, &iblPools.Ring);
    TextureHandle output{};
    {
        DeviceFrame active(*m_Device);
        ASSERT_TRUE(active.Open());
        ASSERT_NO_FATAL_FAILURE(Build(atlas, baseFrame, 1u, guid));
        ASSERT_NO_FATAL_FAILURE(Execute(baseFrame, true));
        output = baseFrame.PhysicalTexture(m_LastAtlas);
    }
    // The front centre is green; the outward normal on the back shell produces red.
    {
        SCOPED_TRACE("IBL off front-facing control");
        ASSERT_NO_FATAL_FAILURE(ExpectColor(output, 1u));
    }
    atlas.SetIblEnabled(true);
    {
        DeviceFrame active(*m_Device);
        ASSERT_TRUE(active.Open());
        ASSERT_NO_FATAL_FAILURE(Build(atlas, iblFrame, 2u, guid));
        ASSERT_NO_FATAL_FAILURE(Execute(iblFrame, true));
        ASSERT_EQ(iblFrame.PhysicalTexture(m_LastAtlas), output);
    }
    {
        SCOPED_TRACE("IBL on front-facing result");
        ASSERT_NO_FATAL_FAILURE(ExpectColor(output, 1u));
    }
}

TEST_P(GraphPreviewAtlasPreparationTest, IblPreviousDrawRemainsPendingUntilCurrentRecordsWithoutAnotherEdit)
{
    // Real shaders carry the color, so shared material parameters cannot hide which unit drew.
    const auto redSurface = m_Cache.Path() / "ibl_red.glsl";
    const auto blueSurface = m_Cache.Path() / "ibl_blue.glsl";
    for (const auto& [path, emission] : {
             std::pair{redSurface, std::string("vec3(2.0,0.0,0.0)")},
             std::pair{blueSurface, std::string("vec3(0.0,0.0,2.0)")}})
    {
        std::ofstream shader(path);
        shader << "SurfaceOutput EvaluateSurface(SurfaceInput sIn) {\n"
               << "SurfaceOutput o = DefaultSurfaceOutput();\n"
               << "o.baseColor = vec3(0.05); o.emissive = " << emission << ";\n"
               << "o.normalWS = normalize(sIn.normalWS); o.opacity = 1.0; return o; }\n";
    }
    auto& ibl = m_Services->EnsureFeature<ImageBasedLightingFeature>();
    ASSERT_TRUE(ibl.Initialize(m_Device.get()));
    m_Device->WaitForIdle();
    ASSERT_TRUE(ibl.IsInitialized());
    ASSERT_TRUE(ibl.GetIrradianceCube().IsValid() && ibl.GetPrefilterCube().IsValid());
    ASSERT_TRUE(ibl.GetBrdfLut().IsValid() && ibl.GetCubeSampler().IsValid() && ibl.GetLutSampler().IsValid());
    MaterialDocument document{};
    document.materialName = "Atlas real IBL generations";
    document.lightingModel = "StandardPBR";
    document.surfaceShader = redSurface.string();
    const GUID guid = GUID::Generate();
    auto* material = m_Services->Materials().RegisterMaterialFromDocument(guid, document);
    ASSERT_NE(material, nullptr);
    ASSERT_TRUE(material->GetGraphicsPipelineId().IsValid());
    const auto* sphere = m_Services->GetMeshGPURegistry().FindByKey({PrimitiveGenerator::SphereGuid(), 0});
    ASSERT_NE(sphere, nullptr);
    const auto flags = sphere->vertexFlags;
    PipelineFormatKey format{};
    format.ColorCount = 1u;
    format.ColorFormats[0] = TextureFormat::R16G16B16A16_FLOAT;
    format.DepthFormat = m_Services->GetDepthFormat();
    format.RasterizationSamples = 1u;
    PipelineVariantCache::VariantEntrySnapshotForTesting firstUnit;
    ASSERT_NO_FATAL_FAILURE(CompileIblVariant(*material, flags, firstUnit));
    const auto warmFirst = m_Device->GetOrCreateGraphicsPipeline(firstUnit.CurrentPipelineId, format);
    ASSERT_TRUE(warmFirst.IsValid());
    const uint32_t index = material->GetGpuSceneMaterialIndex();
    auto& variants = m_Services->Materials().Variants();
    GraphPreviewAtlas atlas;
    atlas.Initialize(m_Services.get());
    atlas.SetIblEnabled(true);
    UiRgHarness firstPools(m_Device.get()), retryPools(m_Device.get()), currentPools(m_Device.get());
    RG::RGFrame first(m_Device.get(), &firstPools.Persistent, &firstPools.Transient, &firstPools.Ring);
    RG::RGFrame retry(m_Device.get(), &retryPools.Persistent, &retryPools.Transient, &retryPools.Ring);
    RG::RGFrame current(m_Device.get(), &currentPools.Persistent, &currentPools.Transient, &currentPools.Ring);
    TextureHandle output{};
    {
        DeviceFrame active(*m_Device);
        ASSERT_TRUE(active.Open());
        ASSERT_NO_FATAL_FAILURE(Build(atlas, first, 1u, guid));
        ASSERT_NO_FATAL_FAILURE(Execute(first, true));
        output = first.PhysicalTexture(m_LastAtlas);
        EXPECT_EQ(variants.ServedGeneration(index, ServedPassClass::Color),
                  std::optional<uint32_t>{firstUnit.MaterialVersion});
    }
    ASSERT_NO_FATAL_FAILURE(ExpectColor(output, 0u));

    document.surfaceShader = blueSurface.string();
    ASSERT_EQ(m_Services->Materials().RegisterMaterialFromDocument(guid, document), material);
    ASSERT_GT(material->GetVersion(), firstUnit.MaterialVersion);
    bool allow = false;
    PipelineVariantCache::VariantEntrySnapshotForTesting replacement;
    {
        DeviceFrame active(*m_Device);
        ASSERT_TRUE(active.Open());
        ASSERT_NO_FATAL_FAILURE(Build(atlas, retry, 2u, guid, &allow));
        // Publish after BeginWorldDrawFrame: no frame-begin warm drain may build Current yet.
        ASSERT_NO_FATAL_FAILURE(CompileIblVariant(*material, flags, replacement));
        ASSERT_NE(replacement.CurrentPipelineId, firstUnit.CurrentPipelineId);
        ASSERT_EQ(replacement.PreviousPipelineId, firstUnit.CurrentPipelineId);
        ASSERT_EQ(replacement.PreviousMaterialVersion, firstUnit.MaterialVersion);
        ASSERT_FALSE(m_Device->TryGetWarmGraphicsPipeline(replacement.CurrentPipelineId, format).IsValid());
        ASSERT_EQ(m_Device->TryGetWarmGraphicsPipeline(replacement.PreviousPipelineId, format), warmFirst);
        ASSERT_NO_FATAL_FAILURE(Execute(retry, false));
        EXPECT_FALSE(variants.ServedGeneration(index, ServedPassClass::Color).has_value());
        ASSERT_NO_FATAL_FAILURE(Execute(retry, false));
        EXPECT_FALSE(variants.ServedGeneration(index, ServedPassClass::Color).has_value());
        ASSERT_FALSE(m_Device->TryGetWarmGraphicsPipeline(replacement.CurrentPipelineId, format).IsValid());
        allow = true;
        ASSERT_NO_FATAL_FAILURE(Execute(retry, true));
        EXPECT_EQ(variants.ServedGeneration(index, ServedPassClass::Color),
                  std::optional<uint32_t>{firstUnit.MaterialVersion});
    }
    ASSERT_NO_FATAL_FAILURE(ExpectColor(output, 0u));

    // Concrete warmth changes; desired material content and requests do not.
    const auto contentEpoch = Material::GetGlobalContentEpoch();
    const auto contentRevision = material->GetContentRevision();
    const auto version = material->GetVersion();
    ASSERT_TRUE(m_Device->GetOrCreateGraphicsPipeline(replacement.CurrentPipelineId, format).IsValid());
    ASSERT_EQ(Material::GetGlobalContentEpoch(), contentEpoch);
    ASSERT_EQ(material->GetContentRevision(), contentRevision);
    ASSERT_EQ(material->GetVersion(), version);
    {
        SCOPED_TRACE("a Previous draw must keep the atlas pending after Current warms without an edit");
        DeviceFrame active(*m_Device);
        ASSERT_TRUE(active.Open());
        ASSERT_NO_FATAL_FAILURE(Build(atlas, current, 3u, guid));
        EXPECT_EQ(Material::GetGlobalContentEpoch(), contentEpoch);
        EXPECT_EQ(material->GetContentRevision(), contentRevision);
        EXPECT_EQ(material->GetVersion(), version);
        ASSERT_NO_FATAL_FAILURE(Execute(current, true));
        EXPECT_EQ(current.PhysicalTexture(m_LastAtlas), output);
        EXPECT_EQ(variants.ServedGeneration(index, ServedPassClass::Color),
                  std::optional<uint32_t>{replacement.MaterialVersion});
    }
    ASSERT_NO_FATAL_FAILURE(ExpectColor(output, 2u));
}

TEST_P(GraphPreviewAtlasPreparationTest, ColdFirstFramePublishesThePlannedAtlasBeforeUiDeclaration)
{
    GraphPreviewAtlas atlas;
    atlas.Initialize(m_Services.get());
    atlas.SetRequests(kWindow, {{"node", m_Red}});
    GraphNodePreviewBinding binding{};
    ASSERT_TRUE(atlas.TryGetBinding("node", binding));
    UIRegistration::RegisterBuiltInControls();
    std::unique_ptr<UIElement> root;
    ASSERT_TRUE(UIParsing::XMLParser::ParseLayoutFromString(
        "<uielement id='root'><uielement id='preview'/></uielement>", root));
    UIManager ui(m_Device.get());
    ui.SetRoot(std::move(root));
    const auto style = m_Cache.Path() / "atlas.css";
    {
        std::ofstream css(style);
        css << "#root { width: 256px; height: 256px; }\n";
        css << "#preview { width: 256px; height: 256px; background-image: engine("
            << binding.Resource << "); }\n";
    }
    ASSERT_TRUE(ui.AttachStyleFromFile(style.string()));
    UiRgHarness pools(m_Device.get());
    RG::RGFrame frame(m_Device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
    TextureHandle output{};
    {
        DeviceFrame active(*m_Device);
        ASSERT_TRUE(active.Open());
        ASSERT_NO_FATAL_FAILURE(Build(atlas, frame, 1u, m_Red, nullptr, nullptr, &ui));
        ui.Update(0.0f, false);
        TextureDesc description{};
        description.width = description.height = 256u;
        description.format = static_cast<uint32_t>(TextureFormat::R16G16B16A16_FLOAT);
        description.usage = static_cast<uint32_t>(TextureUsage::RenderTarget | TextureUsage::ShaderResource);
        const auto target = frame.CreateTexture("Atlas.UiTarget", description);
        ASSERT_TRUE(ui.RenderRG(frame, target, UI::UITargetSpace::LinearSdr()));
        RG::RGPassId uiPass = RG::kInvalidId;
        for (RG::RGPassId pass = 0; pass < frame.Graph().PassCount(); ++pass)
            if (std::string(frame.Graph().PassName(pass)) == "UI Overlay") uiPass = pass;
        ASSERT_NE(uiPass, RG::kInvalidId);
        EXPECT_TRUE(frame.Graph().HasReadAccess(uiPass, m_LastAtlas.Id))
            << "moving publication to Record removes this first-frame dependency";
        frame.MarkOutput(target);
        ASSERT_NO_FATAL_FAILURE(Execute(frame, true));
        output = frame.PhysicalTexture(m_LastAtlas);
    }
    ASSERT_NO_FATAL_FAILURE(ExpectColor(output, 0u));
}

TEST_P(GraphPreviewAtlasPreparationTest, ShutdownBeforeExecuteRefusesTheOwnedGraph)
{
    GraphPreviewAtlas atlas;
    atlas.Initialize(m_Services.get());
    UiRgHarness pools(m_Device.get());
    RG::RGFrame frame(m_Device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
    DeviceFrame active(*m_Device);
    ASSERT_TRUE(active.Open());
    ASSERT_NO_FATAL_FAILURE(Build(atlas, frame, 1u, m_Red));
    atlas.Shutdown();
    ASSERT_NO_FATAL_FAILURE(Execute(frame, false));
}

INSTANTIATE_TEST_SUITE_P(Native, GraphPreviewAtlasPreparationTest,
    ::testing::Values(GraphicsAPI::Vulkan, GraphicsAPI::Metal));
} // namespace
