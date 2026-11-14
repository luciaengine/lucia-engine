#include "Engine/Rendering/ClusterLOD/ClusterLODFeature.h"

#include "Assets/AssetManager.h"
#include "Assets/ClusterLOD/MeshClusterLODAsset.h"
#include "Assets/MeshLODGenerator.h"
#include "ClusterLODTestMesh.h"
#include "Components/Rendering/DDGIVolume.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Transform.h"
#include "Components/Rendering/SkinnedMeshRenderer.h"
#include "Core/DebugMetrics.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/SystemScheduling.h"
#include "Engine/Rendering/ClusterLOD/ClusterLODPrepareSystem.h"
#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/RenderOrigin.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Mathematics/MatrixOps.h"
#include "Rendering/Core/GPUScene.h"
#include "Rendering/Core/RenderGraph/RGResourcePool.h"
#include "Rendering/Core/RenderGraph/RGTransientPool.h"
#include "Rendering/Core/RenderGraph/RGUploadRing.h"
#include "../../Tests/Jobs/HeldWorkers.h"
#include "TestDeviceHelper.h"
#include "../Modules/Rendering/Tests/ScopedEnvVar.h"
#include "TestTempDir.h"
#if defined(RENDERING_HAS_VULKAN)
#include "Vulkan/VulkanDevice/VulkanDevice.h"
#endif

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <string_view>
#include <memory>

namespace GameEngine::Engine::Renderer
{
namespace
{
using namespace GameEngine::Rendering;

class ObserveMeshHandleSystem : public ECS::ISystem
{
  public:
    ObserveMeshHandleSystem(ECS::EntityHandle entity, uint64* handle) : m_Entity(entity), m_Handle(handle) {}
    const char* GetName() const override { return "ObserveMeshHandle"; }
    void Update(ECS::World& world, float32) override
    {
        const auto* renderer = world.GetComponent<Components::MeshRenderer>(m_Entity);
        *m_Handle = renderer ? renderer->meshGpuHandleId : 0;
    }

  private:
    ECS::EntityHandle m_Entity;
    uint64* m_Handle;
};

class ClusterLODRuntimeTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        if (!IsMeshLODGenerationAvailable())
            GTEST_SKIP() << "meshoptimizer not compiled in";
        m_Jobs = std::make_unique<JobSystem::WorkStealingThreadPool>(WorkerCount());
        m_Device = CreateDevice();
        if (!m_Device)
            GTEST_SKIP() << "No Vulkan device available";
        ASSERT_FALSE(m_Device->GetCapabilities().supportsRayQuery);
        const auto& capabilities = m_Device->GetCapabilities();
        if (!capabilities.supportsBufferDeviceAddress || !capabilities.memoryTopology ||
            capabilities.memoryTopology->largestHostVisibleDeviceLocalHeapBytes == 0)
            GTEST_SKIP() << "fine pages require buffer addresses and host-visible device-local memory";
        ASSERT_TRUE(m_Assets.Initialize(m_Jobs.get()));
        ASSERT_TRUE(m_Services.Initialize(m_Device.get()));
        Mesh sourceMesh = Testing::MakeClusterLODGrid();
        sourceMesh.MaterialIndex = 0;
        for (auto& vertex : sourceMesh.Vertices)
            vertex.Position[1] = std::sin(vertex.Position[0] * 0.21f) * std::sin(vertex.Position[2] * 0.19f);
        ModelAsset source(GUID::Generate(), m_Temporary.Path() / "absent.obj");
        source.SetMeshesForTest({std::move(sourceMesh)});
        const auto path = m_Temporary.Path() / "runtime.gecluster";
        MeshClusterLODConfig config;
        config.MeshletsPerGroup = 4;
        String error;
        ASSERT_TRUE(CookMeshClusterLODAsset(source, path, config, error)) << error;
        m_Model = std::make_shared<ModelAsset>(GUID::Generate(), path);
        ASSERT_TRUE(m_Model->Load());
        m_Assets.RegisterLoadedAsset(m_Model->GetGUID(), m_Model);
        const auto manifest = m_Model->GetClusterLODManifest();
        ASSERT_NE(manifest, nullptr);
        auto group = std::find_if(manifest->Pages.begin(), manifest->Pages.end(), [this, manifest](const auto& page)
        {
            const auto index = static_cast<uint32>(&page - manifest->Pages.data());
            return page.IndexCount > m_Model->GetMesh(index).Indices.size();
        });
        ASSERT_NE(group, manifest->Pages.end()) << "the fixture must contain a real coarse reduction";
        m_Submesh = static_cast<uint32>(group - manifest->Pages.begin());
        BeforeCoarseRegistration();
        const auto handles = m_Services.GetMeshGPURegistry().RegisterModelMeshes(m_Model->GetGUID(), *m_Model);
        AfterCoarseRegistration();
        ASSERT_GT(handles.size(), m_Submesh);
        m_Coarse = handles[m_Submesh];
        const auto* coarse = m_Services.GetMeshGPURegistry().Find(m_Coarse);
        ASSERT_NE(coarse, nullptr);
        if (RequiresDrawableCoarse())
        {
            ASSERT_TRUE(m_Services.GetMeshGPURegistry().IsEntryDrawable(*coarse));
        }
        MaterialDocument document;
        document.lightingModel = "StandardPBR";
        document.surfaceShader = "Surfaces/standard_pbr.glsl";
        m_MaterialGuid = GUID::Generate();
        ASSERT_NE(m_Services.Materials().Registry().Register(m_MaterialGuid, document), nullptr);
        Components::MeshRenderer renderer;
        renderer.meshId = m_Submesh;
        renderer.modelAssetGuid.Set(m_Model->GetGUID());
        renderer.materialAssetGuid.Set(m_MaterialGuid);
        renderer.meshGpuHandleId = m_Coarse.id;
        m_Entity = m_World.Create().Set<Components::MeshRenderer>(renderer)
            .Set<Components::WorldTransform>({}).GetHandle();
        m_World.ProcessCommands();
        const auto camera = m_Services.Views().AllocateCamera("Cluster runtime");
        m_View = m_Services.Views().AllocateView("Cluster runtime", camera);
        m_Services.Views().SetViewWorldId(m_View, m_World.GetWorldId());
        m_Services.Views().SetViewRenderLayerMask(m_View, RenderLayerMask::FromLayer(0));
        m_Services.SetLODForceLevel(0);
        m_Feature = &m_Services.EnsureFeature<ClusterLODFeature>();
        ASSERT_TRUE(m_Feature->Initialize(m_Services, m_Assets));
    }

    virtual size_t WorkerCount() const { return 0; }
    virtual std::unique_ptr<IDevice> CreateDevice() { return CreateVulkanDeviceFast(); }
    virtual void BeforeCoarseRegistration() {}
    virtual void AfterCoarseRegistration() {}
    virtual bool RequiresDrawableCoarse() const { return true; }

    void TearDown() override
    {
        if (m_HeldWorkers)
            m_HeldWorkers->Release();
        m_World.Clear();
        m_Services.Shutdown();
        m_Assets.Shutdown();
        if (m_Device)
            m_Device->Shutdown();
        if (m_Jobs)
            m_Jobs->Shutdown();
        m_HeldWorkers.reset();
    }

    void Prepare()
    {
        m_Services.BeginWorldDrawFrame();
        m_Feature->Prepare(m_World);
    }

    MeshGPUHandle CurrentHandle() const
    {
        return MeshGPUHandle(m_World.GetComponent<Components::MeshRenderer>(m_Entity)->meshGpuHandleId);
    }

    Components::MeshRenderer WithOrdinarySource(Components::MeshRenderer renderer)
    {
        auto ordinary = std::make_shared<ModelAsset>(GUID::Generate(), m_Temporary.Path() / "ordinary.obj");
        ordinary->SetMeshesForTest({Testing::MakeClusterLODGrid()});
        m_Assets.RegisterLoadedAsset(ordinary->GetGUID(), ordinary);
        renderer.modelAssetGuid.Set(ordinary->GetGUID());
        return renderer;
    }

    void ExpectFine()
    {
        EXPECT_NE(CurrentHandle(), m_Coarse);
        const auto* entry = m_Services.GetMeshGPURegistry().Find(CurrentHandle());
        ASSERT_NE(entry, nullptr);
        EXPECT_EQ(entry->StorageDomain, MeshGPUStorageDomain::ClusterFine);
        EXPECT_TRUE(m_Services.GetMeshGPURegistry().IsEntryDrawable(*entry));
        EXPECT_EQ(entry->cpuMesh, nullptr) << "CPU picking stays on the stable coarse asset";
    }

    Rendering::Tests::ScopedEnvVar m_SingleQueue{"GE_ASYNC_COMPUTE", "0"};
    Rendering::Tests::ScopedEnvVar m_RasterOnly{"GE_VK_DISABLE_RAY_QUERY", "1"};
    TestUtils::ScopedTempDir m_Temporary{TestUtils::MakeUniqueTempDirectory("cluster-runtime")};
    // The helper outlives pool shutdown even if its acquisition times out.
    std::shared_ptr<GameEngine::Tests::HeldWorkers> m_HeldWorkers;
    std::unique_ptr<JobSystem::WorkStealingThreadPool> m_Jobs;
    AssetManager m_Assets;
    std::unique_ptr<IDevice> m_Device;
    RenderServices m_Services;
    ECS::World m_World;
    std::shared_ptr<ModelAsset> m_Model;
    ClusterLODFeature* m_Feature = nullptr;
    ECS::EntityHandle m_Entity;
    MeshGPUHandle m_Coarse;
    GUID m_MaterialGuid;
    uint32 m_Submesh = 0;
    ViewId m_View = 0;
};

struct ReaderFramePools
{
    RenderGraph::RGResourcePool Persistent;
    RenderGraph::RGTransientPool Transient;
    RenderGraph::RGUploadRing Ring;
    explicit ReaderFramePools(IDevice* device) : Persistent(device), Transient(device), Ring(device, 2, 4096) {}
};

#if defined(RENDERING_HAS_VULKAN)
class GatedGraphicsDevice final : public VulkanDevice
{
  public:
    SemaphoreHandle Gate;
    bool QueueSubmit(QueueType queue, const std::vector<CommandList*>& commands,
                     const std::vector<std::pair<SemaphoreHandle, uint64_t>>& waits,
                     const std::vector<std::pair<SemaphoreHandle, uint64_t>>& signals) override
    {
        auto gated = waits;
        if (queue == QueueType::Graphics && Gate.IsValid())
            gated.emplace_back(Gate, 1);
        return VulkanDevice::QueueSubmit(queue, commands, gated, signals);
    }
};

class ScopedGraphicsGates
{
  public:
    explicit ScopedGraphicsGates(GatedGraphicsDevice& device)
        : Fine(device.CreateTimelineSemaphore()), Later(device.CreateTimelineSemaphore()), m_Device(device) {}
    ~ScopedGraphicsGates()
    {
        m_Device.Gate = {};
        const bool complete = !m_Ready || ReleaseAll();
        EXPECT_TRUE(complete) << "owned GPU gates must drain before graph destruction";
        if (complete)
        {
            m_Device.DestroySemaphore(Fine);
            m_Device.DestroySemaphore(Later);
        }
    }
    bool Initialize()
    {
        if (!Fine.IsValid() || !Later.IsValid())
            return false;
        for (auto& commands : m_Signals)
        {
            commands = m_Device.CreateCommandList(IDevice::QueueType::Compute);
            if (!commands)
                return false;
            commands->Begin();
            commands->End();
        }
        m_Ready = true;
        return true;
    }
    bool ReleaseFine()
    {
        if (m_FineReleased)
            return true;
        m_FineReleased = Fine.IsValid() && m_Device.QueueSubmit(IDevice::QueueType::Compute, {m_Signals[0].get()}, {}, {{Fine, 1}});
        return m_FineReleased;
    }
    bool ReleaseAll()
    {
        const bool fine = ReleaseFine();
        if (!m_LaterReleased)
            m_LaterReleased = Later.IsValid() && m_Device.QueueSubmit(IDevice::QueueType::Compute, {m_Signals[1].get()}, {}, {{Later, 1}});
        if (!fine || !m_LaterReleased)
            return false;
        constexpr uint64 kTimeoutNs = 10'000'000'000ull;
        if (!m_Device.WaitTimelineSemaphoreValue(Fine, 1, kTimeoutNs) ||
            !m_Device.WaitTimelineSemaphoreValue(Later, 1, kTimeoutNs))
            return false;
        const auto last = m_Device.LastGraphicsSubmissionToken();
        return !last.IsValid() || m_Device.WaitGpuSyncToken(last, kTimeoutNs);
    }

    const SemaphoreHandle Fine;
    const SemaphoreHandle Later;

  private:
    GatedGraphicsDevice& m_Device;
    std::array<std::unique_ptr<CommandList>, 2> m_Signals;
    bool m_Ready = false;
    bool m_FineReleased = false;
    bool m_LaterReleased = false;
};

class ClusterLODGpuReaderTest : public ClusterLODRuntimeTest
{
  protected:
    void TearDown() override
    {
        const bool validation = m_Device && m_Device->GetValidationStats().Enabled;
        ClusterLODRuntimeTest::TearDown();
        if (validation)
        {
            EXPECT_EQ(m_Device->GetValidationStats().ErrorCount, 0u) << "includes gate, graph, pool and device cleanup";
            EXPECT_EQ(m_Device->GetValidationStats().WarningCount, 0u);
        }
    }
    std::unique_ptr<IDevice> CreateDevice() override
    {
        auto device = std::make_unique<GatedGraphicsDevice>();
        DeviceDesc descriptor{};
        descriptor.preferredAPI = GraphicsAPI::Vulkan;
        descriptor.enableDynamicRendering = true;
        descriptor.enableDebugLayer = true;
        descriptor.enableSwapchain = false;
        if (!device->Initialize(descriptor))
            return {};
        m_Gated = device.get();
        return device;
    }
    GatedGraphicsDevice* m_Gated = nullptr;
};

constexpr uint32 kFineReadBytes = 64;

void SubmitFineCopy(RenderGraph::RGFrame& frame, RenderServices& services, BufferHandle source,
                    uint64 offset, BufferHandle destination)
{
    frame.BeginFrame(0);
    ASSERT_TRUE(services.Spine().BindWorldGeometry(frame));
    const auto src = frame.ImportExternalBuffer("Fine.Reader.Source", source, offset + kFineReadBytes);
    const auto dst = frame.ImportExternalBuffer("Fine.Reader.Destination", destination, kFineReadBytes);
    frame.AddPass("Fine.Reader.Copy", 0,
        [src, dst](RenderGraph::RGPassBuilder& pass)
        {
            pass.Read(src, RenderGraph::RGBufferRead::CopySrc);
            pass.Write(dst, RenderGraph::RGBufferWrite::CopyDst);
        },
        [src, dst, offset](RenderGraph::RGContext& context)
        {
            context.Cmd->CopyBuffer(context.GetBuffer(src), context.GetBuffer(dst), kFineReadBytes, offset, 0);
        });
    frame.MarkOutput(dst);
    frame.Execute();
    ASSERT_FALSE(frame.Stats().ExecutionRejected);
    ASSERT_GT(frame.Stats().SubmissionsMade, 0u);
}

TEST_F(ClusterLODGpuReaderTest, TwoSubmittedReadersKeepFineStorageUntilTheirFixedTokenCompletes)
{
    if (!m_Device->GetValidationStats().Enabled)
        GTEST_SKIP() << "Vulkan validation layer unavailable for the real queue-lifetime witness";
    ASSERT_EQ(m_Device->GetValidationStats().ErrorCount, 0u);
    if (m_Device->GetComputeQueueFamilyIndex() == m_Device->GetGraphicsQueueFamilyIndex())
        GTEST_SKIP() << "GPU gate release requires a physically independent compute queue family";
    Prepare();
    Prepare();
    ASSERT_NO_FATAL_FAILURE(ExpectFine());
    const auto fine = CurrentHandle();
    const auto* entry = m_Services.GetMeshGPURegistry().Find(fine);
    ASSERT_NE(entry, nullptr);
    ASSERT_GE(entry->coreSubAlloc.Size, kFineReadBytes);
    const uint64 offset = entry->coreSubAlloc.Offset;
    MeshGPUEntryBindings physical;
    ASSERT_TRUE(m_Services.GetMeshGPURegistry().TryGetDrawableBindings(*entry, physical));
    std::array<uint8, kFineReadBytes> expected;
    const auto* mapped = static_cast<const uint8*>(m_Device->MapBuffer(physical.coreVB));
    ASSERT_NE(mapped, nullptr);
    std::memcpy(expected.data(), mapped + offset, expected.size());
    m_Device->UnmapBuffer(physical.coreVB);
    std::array<BufferHandle, 2> readbacks;
    BufferDesc descriptor{};
    descriptor.size = kFineReadBytes;
    descriptor.usage = static_cast<uint32>(BufferUsage::TransferDst);
    descriptor.memoryUsage = BufferMemoryUsage::Readback;
    descriptor.flags = BufferCreateFlags::PersistentlyMapped;
    descriptor.debugName = "ClusterLOD.Reader.Readback";
    for (auto& buffer : readbacks)
    {
        buffer = m_Device->CreateBuffer(descriptor);
        ASSERT_TRUE(buffer.IsValid());
    }
    const auto drains = m_Device->GetIdleDrainCount();
    m_Device->WaitForIdle();
    ASSERT_EQ(m_Device->GetIdleDrainCount(), drains + 1) << "the no-drain oracle must move";
    const auto baselineDrains = m_Device->GetIdleDrainCount();
    ReaderFramePools poolsA(m_Device.get()), poolsB(m_Device.get()), poolsLater(m_Device.get());
    RenderGraph::RGFrame first(m_Device.get(), &poolsA.Persistent, &poolsA.Transient, &poolsA.Ring);
    RenderGraph::RGFrame second(m_Device.get(), &poolsB.Persistent, &poolsB.Transient, &poolsB.Ring);
    RenderGraph::RGFrame later(m_Device.get(), &poolsLater.Persistent, &poolsLater.Transient, &poolsLater.Ring);
    ScopedGraphicsGates gates(*m_Gated);
    ASSERT_TRUE(gates.Initialize());
    ASSERT_TRUE(gates.Fine.IsValid());
    ASSERT_TRUE(gates.Later.IsValid());
    m_Gated->Gate = gates.Fine;
    ASSERT_NO_FATAL_FAILURE(SubmitFineCopy(first, m_Services, physical.coreVB, offset, readbacks[0]));
    ASSERT_NO_FATAL_FAILURE(SubmitFineCopy(second, m_Services, physical.coreVB, offset, readbacks[1]));
    const auto readers = m_Device->LastGraphicsSubmissionToken();
    ASSERT_TRUE(readers.IsValid());
    ASSERT_EQ(m_Device->QueryGpuSyncToken(readers), IDevice::GpuSyncStatus::Pending);
    m_Services.SetLODForceLevel(1);
    for (uint32 frame = 0; frame < 35; ++frame)
        Prepare();
    entry = m_Services.GetMeshGPURegistry().Find(fine);
    ASSERT_NE(entry, nullptr) << "submitted readers still wait on the owned GPU gate";
    ASSERT_TRUE(entry->DrawClosed);
    EXPECT_EQ(m_Device->QueryGpuSyncToken(readers), IDevice::GpuSyncStatus::Pending);
    m_Gated->Gate = gates.Later;
    later.BeginFrame(0);
    ASSERT_TRUE(m_Services.Spine().BindWorldGeometry(later));
    later.AddPass("Later.Blocked", 0, [](RenderGraph::RGPassBuilder& pass) { pass.PreventCulling(); },
                  [](RenderGraph::RGContext&) {});
    later.Execute();
    ASSERT_GT(later.Stats().SubmissionsMade, 0u);
    const auto newest = m_Device->LastGraphicsSubmissionToken();
    ASSERT_EQ(newest.sem, readers.sem);
    ASSERT_GT(newest.value, readers.value);
    ASSERT_TRUE(gates.ReleaseFine());
    ASSERT_TRUE(GameEngine::Tests::WaitUntil([this, fine]
    {
        Prepare();
        return m_Services.GetMeshGPURegistry().Find(fine) == nullptr;
    }, std::chrono::seconds(10)));
    EXPECT_EQ(m_Device->QueryGpuSyncToken(readers), IDevice::GpuSyncStatus::Complete);
    EXPECT_EQ(m_Device->QueryGpuSyncToken(newest), IDevice::GpuSyncStatus::Pending)
        << "retirement must use its closed-reader token while later unrelated graphics remain blocked";
    EXPECT_EQ(m_Device->GetIdleDrainCount(), baselineDrains);
    for (const auto buffer : readbacks)
    {
        const auto* copied = static_cast<const uint8*>(m_Device->MapBuffer(buffer));
        ASSERT_NE(copied, nullptr);
        EXPECT_EQ(std::memcmp(copied, expected.data(), expected.size()), 0);
        m_Device->UnmapBuffer(buffer);
    }
    ASSERT_TRUE(gates.ReleaseAll());
    for (const auto buffer : readbacks)
        m_Device->DestroyBuffer(buffer);
    EXPECT_EQ(m_Device->GetValidationStats().ErrorCount, 0u);
    EXPECT_EQ(m_Device->GetValidationStats().WarningCount, 0u);
}

class CoarseRefusingDevice final : public VulkanDevice
{
  public:
    bool RefuseCore = false;
    uint32 Refusals = 0;
    BufferHandle CreateBuffer(const BufferDesc& descriptor) override
    {
        if (RefuseCore && descriptor.debugName &&
            std::string_view(descriptor.debugName) == "MeshGPU.Bucket.CoreVB")
        {
            ++Refusals;
            return {};
        }
        return VulkanDevice::CreateBuffer(descriptor);
    }
};

class ClusterLODCoarseFailureTest : public ClusterLODRuntimeTest
{
  protected:
    std::unique_ptr<IDevice> CreateDevice() override
    {
        auto device = std::make_unique<CoarseRefusingDevice>();
        DeviceDesc descriptor{};
        descriptor.preferredAPI = GraphicsAPI::Vulkan;
        descriptor.enableDynamicRendering = true;
        descriptor.enableSwapchain = false;
        if (!device->Initialize(descriptor))
            return {};
        m_Refusing = device.get();
        return device;
    }
    void BeforeCoarseRegistration() override { m_Refusing->RefuseCore = true; }
    void AfterCoarseRegistration() override { m_Refusing->RefuseCore = false; }
    bool RequiresDrawableCoarse() const override { return false; }
    CoarseRefusingDevice* m_Refusing = nullptr;
};

TEST_F(ClusterLODCoarseFailureTest, FailedCoarseCoverageRefusesFineEvenWhenFineStorageIsAvailable)
{
    ASSERT_GT(m_Refusing->Refusals, 0u);
    ASSERT_FALSE(m_Services.GetMeshGPURegistry().IsEntryDrawable(*m_Services.GetMeshGPURegistry().Find(m_Coarse)));
    const auto control = m_Services.GetMeshGPURegistry().RegisterSubmesh({GUID::Generate(), 0},
        m_Model->GetMesh(m_Submesh), false, MeshGPUStorageDomain::ClusterFine);
    const auto* entry = m_Services.GetMeshGPURegistry().Find(control);
    ASSERT_NE(entry, nullptr);
    ASSERT_TRUE(m_Services.GetMeshGPURegistry().IsEntryDrawable(*entry)) << "fine admission must be physically possible";
    const auto entries = m_Services.GetMeshGPURegistry().GetEntryCount();
    Prepare();
    Prepare();
    EXPECT_EQ(CurrentHandle(), m_Coarse);
    EXPECT_EQ(m_Services.GetMeshGPURegistry().GetEntryCount(), entries);
}
#endif

class ClusterLODAsyncTest : public ClusterLODRuntimeTest
{
  protected:
    size_t WorkerCount() const override { return 1; }
};

TEST_F(ClusterLODAsyncTest, AQueuedDecodeKeepsCoarseFramesProgressingUntilTheWorkerIsReleased)
{
    ASSERT_EQ(m_Jobs->GetBackgroundQueuedForTests(), 0u);
    m_HeldWorkers = std::make_shared<GameEngine::Tests::HeldWorkers>(*m_Jobs);
    ASSERT_FALSE(HasFailure());
    Prepare();
    ASSERT_TRUE(GameEngine::Tests::WaitUntil([this] { return m_Jobs->GetBackgroundQueuedForTests() > 0; },
                                            std::chrono::seconds(10)))
        << "the real IO reader must have submitted the fine decode behind the held worker";
    for (uint32 frame = 0; frame < 3; ++frame)
    {
        Prepare();
        EXPECT_EQ(CurrentHandle(), m_Coarse);
    }
    m_HeldWorkers->Release();
    ASSERT_TRUE(GameEngine::Tests::WaitUntil([this]
    {
        Prepare();
        const auto* entry = m_Services.GetMeshGPURegistry().Find(CurrentHandle());
        return entry && entry->StorageDomain == MeshGPUStorageDomain::ClusterFine;
    }, std::chrono::seconds(10)));
    ASSERT_NO_FATAL_FAILURE(ExpectFine());
}

class ClusterLODMultipleViewTest : public ClusterLODRuntimeTest,
                                   public ::testing::WithParamInterface<bool>
{
  protected:
    ViewId AddSseView(const Vector3& eye, const Vector3& center)
    {
        CameraData camera{};
        const auto projection = Mathematics::MakePerspectiveLH_ZO_ReverseZ(
            1.0471976f, 1.5f, 0.1f, 2'000'000.0f);
        const auto view = Mathematics::MakeLookAtLH(eye, center, Vector3(0, 1, 0));
        camera.proj = projection;
        camera.view = view;
        const auto viewProjection = projection * view;
        camera.viewProj = viewProjection;
        SetCameraEye(camera, Mathematics::WorldPosition::FromVector3(eye));
        auto& views = m_Services.Views();
        const auto cameraId = views.AllocateCamera("Cluster SSE union");
        views.SetCameraData(cameraId, camera, CameraRanges{0.1f, 2'000'000.0f});
        const auto id = views.AllocateView("Cluster SSE union", cameraId);
        views.SetViewWorldId(id, m_World.GetWorldId());
        views.SetViewRenderLayerMask(id, RenderLayerMask::FromLayer(0));
        views.PerView(id).WorldViewportHeight = 1080;
        return id;
    }
};

TEST_P(ClusterLODMultipleViewTest, NearDemandSurvivesFarViewAndRecoversAfterViewRemoval)
{
    ASSERT_TRUE(m_Services.Views().ReleaseView(m_View));
    m_Services.SetLODForceLevel(~0u);
    m_Services.GetMeshGPURegistry().SetLodSelectionMode(LodSelectionMode::Sse);
    m_Services.SetLODErrorBudgetPx(1.0f);
    ASSERT_GT(m_Model->GetClusterLODManifest()->Pages[m_Submesh].Error, 0.0f);
    const auto& mesh = m_Model->GetMesh(m_Submesh);
    const Vector3 low(mesh.MinBounds[0], mesh.MinBounds[1], mesh.MinBounds[2]);
    const Vector3 high(mesh.MaxBounds[0], mesh.MaxBounds[1], mesh.MaxBounds[2]);
    const Vector3 center = (low + high) * 0.5f;
    const Vector3 nearEye = center + Vector3(0, 2, -2);
    const Vector3 farEye = center + Vector3(0, 0, -1'000'000);
    const auto first = AddSseView(GetParam() ? nearEye : farEye, center);
    const auto second = AddSseView(GetParam() ? farEye : nearEye, center);
    const auto nearView = GetParam() ? first : second;
    m_Services.Views().SetViewRenderLayerMask(nearView, RenderLayerMask::None());
    Prepare();
    Prepare();
    ASSERT_EQ(CurrentHandle(), m_Coarse) << "the far view alone must select coarse";

    m_Services.Views().SetViewRenderLayerMask(nearView, RenderLayerMask::FromLayer(0));
    Prepare();
    Prepare();
    ASSERT_NO_FATAL_FAILURE(ExpectFine());
    ASSERT_TRUE(m_Services.Views().ReleaseView(nearView));
    Prepare();
    ASSERT_EQ(CurrentHandle(), m_Coarse);
    for (uint32 frame = 0; frame < 35; ++frame)
        Prepare();
    AddSseView(nearEye, center);
    Prepare();
    Prepare();
    ASSERT_NO_FATAL_FAILURE(ExpectFine());
}

INSTANTIATE_TEST_SUITE_P(ViewRegistrationOrder, ClusterLODMultipleViewTest, ::testing::Bool());

TEST_F(ClusterLODRuntimeTest, TheNextScheduledSystemSeesFineAndCoarseInTheSameCycle)
{
    uint64 observed = 0;
    ECS::SystemManager manager(m_Jobs.get());
    ECS::SystemScheduleBuilder schedule;
    schedule.Add<ClusterLODPrepareSystem>("ClusterLODPrepare", ECS::SystemPhase::Extraction, 0, {}, m_Feature);
    schedule.Add<ObserveMeshHandleSystem>("Observe", ECS::SystemPhase::Extraction, 1,
                                         {"ClusterLODPrepare"}, m_Entity, &observed);
    schedule.BuildAndRegisterWithWaves(manager);
    m_Services.BeginWorldDrawFrame();
    manager.Update(m_World, 0.0f);
    EXPECT_EQ(observed, m_Coarse.id) << "pending fine IO must preserve the complete coarse cover";
    m_Services.BeginWorldDrawFrame();
    manager.Update(m_World, 0.0f);
    ASSERT_NO_FATAL_FAILURE(ExpectFine());
    EXPECT_EQ(observed, CurrentHandle().id) << "no command flush may be needed between these systems";
    m_Services.SetLODForceLevel(1);
    m_Services.BeginWorldDrawFrame();
    manager.Update(m_World, 0.0f);
    EXPECT_EQ(CurrentHandle(), m_Coarse);
    EXPECT_EQ(observed, m_Coarse.id);
}

TEST_F(ClusterLODRuntimeTest, CoarseReRegistrationRefreshesTheCachedFallbackHandle)
{
    Prepare();
    Prepare();
    ASSERT_NO_FATAL_FAILURE(ExpectFine());
    const MeshGPUKey source{m_Model->GetGUID(), m_Submesh};
    ASSERT_TRUE(m_Services.GetMeshGPURegistry().UnregisterSubmesh(source));
    m_Services.SetLODForceLevel(1);
    Prepare();
    const auto refreshed = m_Services.GetMeshGPURegistry().FindHandle(source);
    EXPECT_TRUE(refreshed.IsValid());
    EXPECT_NE(refreshed, m_Coarse);
    EXPECT_EQ(CurrentHandle(), refreshed);
    const auto* entry = m_Services.GetMeshGPURegistry().Find(refreshed);
    ASSERT_NE(entry, nullptr);
    EXPECT_TRUE(m_Services.GetMeshGPURegistry().IsEntryDrawable(*entry));
}

TEST_F(ClusterLODRuntimeTest, CoarseOnlyDemandDoesNotOccupyTheBoundedFinePageDirectory)
{
    m_Services.SetLODForceLevel(1);
    Prepare();
    EXPECT_EQ(CurrentHandle(), m_Coarse);
#if GE_ENABLE_METRICS
    float tracked = -1.0f;
    ASSERT_TRUE(Debug::DebugMetrics::Get().GetLatest("ClusterLOD.TrackedPages", tracked));
    EXPECT_EQ(tracked, 0.0f);
#endif
    m_Services.SetLODForceLevel(0);
    Prepare();
    Prepare();
    ASSERT_NO_FATAL_FAILURE(ExpectFine());
}

TEST_F(ClusterLODRuntimeTest, MissingFineStoreKeepsCoarseCoverageAndFinishesTheRead)
{
    const auto store = ResolveMeshClusterLODStore(m_Model->GetPath(), {}, *m_Model->GetClusterLODManifest());
    const auto saved = store.string() + ".saved";
    std::filesystem::rename(store, saved);
    Prepare();
    Prepare();
    EXPECT_EQ(CurrentHandle(), m_Coarse);
    EXPECT_EQ(m_Services.GetMeshGPURegistry().GetStorageDomainCapacityBytes(MeshGPUStorageDomain::ClusterFine), 0u);
    EXPECT_TRUE(m_Services.GetMeshGPURegistry().IsEntryDrawable(*m_Services.GetMeshGPURegistry().Find(m_Coarse)));
#if GE_ENABLE_METRICS
    float pending = -1.0f;
    float charge = -1.0f;
    ASSERT_TRUE(Debug::DebugMetrics::Get().GetLatest("ClusterLOD.PendingReads", pending));
    ASSERT_TRUE(Debug::DebugMetrics::Get().GetLatest("ClusterLOD.ChargedPageWorkBytes", charge));
    EXPECT_EQ(pending, 0.0f);
    EXPECT_EQ(charge, 0.0f);
#endif
    std::filesystem::rename(saved, store);
    for (uint32 frame = 0; frame < 130; ++frame)
        Prepare();
    ASSERT_NO_FATAL_FAILURE(ExpectFine());
}

TEST_F(ClusterLODRuntimeTest, LiveOpaqueToBlendAndBackChangesOnlyTheCachedGeometry)
{
    Prepare();
    Prepare();
    ASSERT_NO_FATAL_FAILURE(ExpectFine());
    MaterialDocument blend;
    blend.lightingModel = "StandardPBR";
    blend.surfaceShader = "Surfaces/standard_pbr.glsl";
    blend.alphaMode = MaterialAlphaMode::Blend;
    const GUID blendGuid = GUID::Generate();
    ASSERT_NE(m_Services.Materials().Registry().Register(blendGuid, blend), nullptr);
    m_World.GetComponentForWrite<Components::MeshRenderer>(m_Entity)->materialAssetGuid.Set(blendGuid);
    Prepare();
    EXPECT_EQ(CurrentHandle(), m_Coarse);
    EXPECT_EQ(m_World.GetComponent<Components::MeshRenderer>(m_Entity)->materialAssetGuid.ToGuid(), blendGuid);
    m_World.GetComponentForWrite<Components::MeshRenderer>(m_Entity)->materialAssetGuid.Set(m_MaterialGuid);
    Prepare();
    ASSERT_NO_FATAL_FAILURE(ExpectFine());
    EXPECT_EQ(m_World.GetComponent<Components::MeshRenderer>(m_Entity)->materialAssetGuid.ToGuid(), m_MaterialGuid);
}

TEST_F(ClusterLODRuntimeTest, ANewSkinComponentRestoresCoarseBeforeTheNextExtraction)
{
    Prepare();
    Prepare();
    ASSERT_NO_FATAL_FAILURE(ExpectFine());
    m_World.AddComponent(m_Entity, Components::SkinnedMeshRenderer{});
    m_World.ProcessCommands();
    Prepare();
    EXPECT_EQ(CurrentHandle(), m_Coarse);
}

TEST_F(ClusterLODRuntimeTest, AnAbandonedArenaReleasesDecodedOwnershipAndCannotReplayAReusedRow)
{
    Prepare();
    Prepare();
    ASSERT_NO_FATAL_FAILURE(ExpectFine());
    const auto fine = CurrentHandle();
    const uint32 oldRow = m_Services.GetMeshGPURegistry().Find(fine)->gpuMeshIndex;
#if GE_ENABLE_METRICS
    float decoded = 0.0f;
    ASSERT_TRUE(Debug::DebugMetrics::Get().GetLatest("ClusterLOD.DecodedElementBytes", decoded));
    ASSERT_GT(decoded, 0.0f) << "the ownership witness needs a live decoded payload";
#endif
    ReaderFramePools pools(m_Device.get());
    RenderGraph::RGFrame abandoned(m_Device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
    abandoned.BeginFrame(0);
    ASSERT_TRUE(m_Services.Spine().BindWorldGeometry(abandoned));
    uint32 recorded = 0;
    abandoned.AddPass("Abandoned.Fine", 0,
        [](RenderGraph::RGPassBuilder& pass) { pass.PreventCulling(); },
        [&recorded](RenderGraph::RGContext&) { ++recorded; });
    m_Services.SetLODForceLevel(1);
    for (uint32 frame = 0; frame < 35; ++frame)
        Prepare();
    EXPECT_EQ(CurrentHandle(), m_Coarse);
    EXPECT_EQ(m_Services.GetMeshGPURegistry().Find(fine), nullptr);
#if GE_ENABLE_METRICS
    float charge = -1.0f;
    ASSERT_TRUE(Debug::DebugMetrics::Get().GetLatest("ClusterLOD.ChargedPageWorkBytes", charge));
    EXPECT_EQ(charge, 0.0f);
    ASSERT_TRUE(Debug::DebugMetrics::Get().GetLatest("ClusterLOD.DecodedElementBytes", decoded));
    EXPECT_EQ(decoded, 0.0f) << "the decoded payload's deleter must run despite the retained arena";
#endif
    m_Services.SetLODForceLevel(0);
    Prepare();
    Prepare();
    ASSERT_NO_FATAL_FAILURE(ExpectFine());
    const auto* replacement = m_Services.GetMeshGPURegistry().Find(CurrentHandle());
    ASSERT_NE(replacement, nullptr);
    EXPECT_EQ(replacement->gpuMeshIndex, oldRow) << "the witness must exercise numeric row reuse";
    abandoned.Execute();
    EXPECT_TRUE(abandoned.Stats().ExecutionRejected);
    EXPECT_EQ(recorded, 0u);
    abandoned.DiscardRecordedPasses();
}

TEST_F(ClusterLODRuntimeTest, AnOrdinarySecondWorldResealsItsOwnAppEpoch)
{
    Prepare();
    Prepare();
    ASSERT_NO_FATAL_FAILURE(ExpectFine());
    ECS::World preview;
    m_Services.BeginWorldDrawFrame();
    m_Feature->Prepare(preview);
    RenderGraph::RGFrame frame(m_Device.get(), nullptr, nullptr, nullptr);
    EXPECT_TRUE(m_Services.Spine().BindWorldGeometry(frame));
    ASSERT_NO_FATAL_FAILURE(ExpectFine());
    frame.DiscardRecordedPasses();
}

TEST_F(ClusterLODRuntimeTest, PreviewPreparationDoesNotSuppressOwnerSelectionInTheSameEpoch)
{
    Prepare();
    Prepare();
    ASSERT_NO_FATAL_FAILURE(ExpectFine());
    ASSERT_NE(CurrentHandle(), m_Coarse);
    ECS::World preview;
    m_Services.BeginWorldDrawFrame();
    const uint64 epoch = m_Services.Spine().WorldFrameEpoch();
    m_Services.SetLODForceLevel(1);
    m_Feature->Prepare(preview);

    ReaderFramePools pools(m_Device.get());
    RenderGraph::RGFrame earlier(m_Device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
    earlier.BeginFrame(0);
    ASSERT_TRUE(m_Services.Spine().BindWorldGeometry(earlier));
    uint32 recorded = 0;
    earlier.AddPass("Preview.BeforeOwnerSelection", 0,
        [](RenderGraph::RGPassBuilder& pass) { pass.PreventCulling(); },
        [&recorded](RenderGraph::RGContext&) { ++recorded; });

    m_Feature->Prepare(m_World);
    EXPECT_EQ(m_Services.Spine().WorldFrameEpoch(), epoch);
    EXPECT_EQ(CurrentHandle(), m_Coarse);
    earlier.Execute();
    EXPECT_TRUE(earlier.Stats().ExecutionRejected);
    EXPECT_EQ(recorded, 0u);
    earlier.DiscardRecordedPasses();

    RenderGraph::RGFrame fresh(m_Device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
    fresh.BeginFrame(1);
    ASSERT_TRUE(m_Services.Spine().BindWorldGeometry(fresh));
    uint32 freshRecorded = 0;
    fresh.AddPass("Preview.AfterOwnerSelection", 0,
        [](RenderGraph::RGPassBuilder& pass) { pass.PreventCulling(); },
        [&freshRecorded](RenderGraph::RGContext&) { ++freshRecorded; });
    fresh.Execute();
    EXPECT_FALSE(fresh.Stats().ExecutionRejected);
    EXPECT_EQ(freshRecorded, 1u);
    fresh.DiscardRecordedPasses();
}

TEST_F(ClusterLODRuntimeTest, PreviewPreparationDoesNotSuppressOwnerMaterialRestorationInTheSameEpoch)
{
    Prepare();
    Prepare();
    ASSERT_NO_FATAL_FAILURE(ExpectFine());
    ASSERT_NE(CurrentHandle(), m_Coarse);
    MaterialDocument blend;
    blend.lightingModel = "StandardPBR";
    blend.surfaceShader = "Surfaces/standard_pbr.glsl";
    blend.alphaMode = MaterialAlphaMode::Blend;
    const GUID blendGuid = GUID::Generate();
    ASSERT_NE(m_Services.Materials().Registry().Register(blendGuid, blend), nullptr);
    m_World.GetComponentForWrite<Components::MeshRenderer>(m_Entity)->materialAssetGuid.Set(blendGuid);
    ECS::World preview;
    m_Services.BeginWorldDrawFrame();
    const uint64 epoch = m_Services.Spine().WorldFrameEpoch();
    m_Feature->Prepare(preview);
    m_Feature->Prepare(m_World);
    EXPECT_EQ(m_Services.Spine().WorldFrameEpoch(), epoch);
    EXPECT_EQ(CurrentHandle(), m_Coarse);
    EXPECT_EQ(m_World.GetComponent<Components::MeshRenderer>(m_Entity)->materialAssetGuid.ToGuid(), blendGuid);
}

TEST_F(ClusterLODRuntimeTest, LateGiRestoresTheLiveHandleBeforeExtraction)
{
    Prepare();
    Prepare();
    ASSERT_NO_FATAL_FAILURE(ExpectFine());
    m_World.Create().Set<Components::DDGIVolume>({}).Set<Components::WorldTransform>({});
    m_World.ProcessCommands();
    Prepare();
    EXPECT_EQ(CurrentHandle(), m_Coarse);
}

TEST_F(ClusterLODRuntimeTest, DisabledOwnerWorldCloneRestoresItsCopiedFineCache)
{
    Prepare();
    Prepare();
    ASSERT_NO_FATAL_FAILURE(ExpectFine());
    const auto fine = CurrentHandle();
    const auto clone = ECS::Entity(&m_World, m_Entity).Clone().SetEnabled(false).GetHandle();
    m_World.ProcessCommands();
    const auto* renderer = m_World.GetComponent<Components::MeshRenderer>(clone);
    ASSERT_NE(renderer, nullptr);
    ASSERT_EQ(renderer->meshGpuHandleId, fine.id);
    Prepare();
    EXPECT_EQ(m_World.GetComponent<Components::MeshRenderer>(clone)->meshGpuHandleId, m_Coarse.id);
    EXPECT_EQ(CurrentHandle(), fine) << "the active original still requests fine geometry";
}

TEST_F(ClusterLODRuntimeTest, OwnerWorldCloneWithOrdinarySourceClearsItsCopiedFineCache)
{
    Prepare();
    Prepare();
    ASSERT_NO_FATAL_FAILURE(ExpectFine());
    const auto fine = CurrentHandle();
    const auto clone = ECS::Entity(&m_World, m_Entity).Clone().GetHandle();
    m_World.ProcessCommands();
    const auto* renderer = m_World.GetComponent<Components::MeshRenderer>(clone);
    ASSERT_NE(renderer, nullptr);
    ASSERT_EQ(renderer->meshGpuHandleId, fine.id);
    const auto changed = WithOrdinarySource(*renderer);
    ECS::Entity(&m_World, clone).Set<Components::MeshRenderer>(changed);
    m_World.ProcessCommands();
    Prepare();
    EXPECT_EQ(m_World.GetComponent<Components::MeshRenderer>(clone)->meshGpuHandleId, 0u);
    EXPECT_EQ(CurrentHandle(), fine) << "the active original still requests fine geometry";
}

TEST_F(ClusterLODRuntimeTest, SecondClusterWorldRestoresCopiedFineCachesIncludingDisabledEntities)
{
    Prepare();
    Prepare();
    ASSERT_NO_FATAL_FAILURE(ExpectFine());
    const auto copied = *m_World.GetComponent<Components::MeshRenderer>(m_Entity);
    ECS::World preview;
    const auto active = preview.Create().Set<Components::MeshRenderer>(copied)
        .Set<Components::WorldTransform>({}).GetHandle();
    const auto disabled = preview.Create().Set<Components::MeshRenderer>(copied)
        .Set<Components::WorldTransform>({}).SetEnabled(false).GetHandle();
    preview.ProcessCommands();
    m_Services.BeginWorldDrawFrame();
    m_Feature->Prepare(preview);
    EXPECT_EQ(CurrentHandle(), m_Coarse);
    EXPECT_EQ(preview.GetComponent<Components::MeshRenderer>(active)->meshGpuHandleId, m_Coarse.id);
    EXPECT_EQ(preview.GetComponent<Components::MeshRenderer>(disabled)->meshGpuHandleId, m_Coarse.id);
    RenderGraph::RGFrame frame(m_Device.get(), nullptr, nullptr, nullptr);
    EXPECT_TRUE(m_Services.Spine().BindWorldGeometry(frame));
    frame.DiscardRecordedPasses();
}

TEST_F(ClusterLODRuntimeTest, LateFineInstanceInvalidatesAnAlreadySealedCoarseCut)
{
    Prepare();
    Prepare();
    ASSERT_NO_FATAL_FAILURE(ExpectFine());
    const auto* fine = m_Services.GetMeshGPURegistry().Find(CurrentHandle());
    ASSERT_NE(fine, nullptr);
    const auto fineRow = fine->gpuMeshIndex;
    m_World.Create().Set<Components::DDGIVolume>({}).Set<Components::WorldTransform>({});
    m_World.ProcessCommands();
    Prepare();
    ASSERT_EQ(CurrentHandle(), m_Coarse);
    RenderGraph::RGFrame earlier(m_Device.get(), nullptr, nullptr, nullptr);
    ASSERT_TRUE(m_Services.Spine().BindWorldGeometry(earlier));
    auto* scene = m_Services.GetGPUScene();
    ASSERT_NE(scene, nullptr);
    GPUInstance instance{};
    instance.meshIndex = fineRow;
    const auto row = scene->AddInstance(instance);
    RenderGraph::RGFrame later(m_Device.get(), nullptr, nullptr, nullptr);
    EXPECT_FALSE(m_Services.Spine().BindWorldGeometry(later));
    earlier.Execute();
    EXPECT_TRUE(earlier.Stats().ExecutionRejected);
    scene->RemoveInstance(row);
    earlier.DiscardRecordedPasses();
    later.DiscardRecordedPasses();
}

TEST_F(ClusterLODRuntimeTest, DisabledOnlyForeignFineCacheTriggersCoarseFallback)
{
    Prepare();
    Prepare();
    ASSERT_NO_FATAL_FAILURE(ExpectFine());
    const auto copied = *m_World.GetComponent<Components::MeshRenderer>(m_Entity);
    ECS::World preview;
    const auto entity = preview.Create().Set<Components::MeshRenderer>(copied)
        .Set<Components::WorldTransform>({}).SetEnabled(false).GetHandle();
    preview.ProcessCommands();
    m_Services.BeginWorldDrawFrame();
    m_Feature->Prepare(preview);
    EXPECT_EQ(CurrentHandle(), m_Coarse);
    EXPECT_EQ(preview.GetComponent<Components::MeshRenderer>(entity)->meshGpuHandleId, m_Coarse.id);
}

TEST_F(ClusterLODRuntimeTest, ForeignFineCacheWithChangedSourceIsClearedBeforeExtraction)
{
    Prepare();
    Prepare();
    ASSERT_NO_FATAL_FAILURE(ExpectFine());
    const auto copied = WithOrdinarySource(*m_World.GetComponent<Components::MeshRenderer>(m_Entity));
    ECS::World preview;
    const auto entity = preview.Create().Set<Components::MeshRenderer>(copied)
        .Set<Components::WorldTransform>({}).GetHandle();
    preview.ProcessCommands();
    m_Services.BeginWorldDrawFrame();
    m_Feature->Prepare(preview);
    EXPECT_EQ(CurrentHandle(), m_Coarse);
    EXPECT_EQ(preview.GetComponent<Components::MeshRenderer>(entity)->meshGpuHandleId, 0u);
}

TEST_F(ClusterLODRuntimeTest, PendingForeignCoverSurvivesOwnerEpochsAndRetirementGrace)
{
    Prepare();
    Prepare();
    ASSERT_NO_FATAL_FAILURE(ExpectFine());
    const auto fine = CurrentHandle();
    const auto copied = *m_World.GetComponent<Components::MeshRenderer>(m_Entity);
    ECS::World preview;
    const auto entity = preview.Create().Set<Components::MeshRenderer>(copied)
        .Set<Components::WorldTransform>({}).SetEnabled(false).GetHandle();
    preview.ProcessCommands();
    const MeshGPUKey source{m_Model->GetGUID(), m_Submesh};
    auto& registry = m_Services.GetMeshGPURegistry();
    ASSERT_TRUE(registry.UnregisterSubmesh(source));
    m_Services.BeginWorldDrawFrame();
    m_Feature->Prepare(preview);
    ASSERT_EQ(preview.GetComponent<Components::MeshRenderer>(entity)->meshGpuHandleId, fine.id);
    m_World.Clear();
    for (uint32 epoch = 0; epoch < 35; ++epoch)
    {
        Prepare();
        RenderGraph::RGFrame blocked(m_Device.get(), nullptr, nullptr, nullptr);
        EXPECT_FALSE(m_Services.Spine().BindWorldGeometry(blocked));
        blocked.DiscardRecordedPasses();
    }
    EXPECT_NE(registry.Find(fine), nullptr) << "unresolved cached cover must retain its page mapping";
    const auto repaired = registry.RegisterSubmesh(source, m_Model->GetMesh(m_Submesh));
    const auto* entry = registry.Find(repaired);
    ASSERT_NE(entry, nullptr);
    ASSERT_TRUE(registry.IsEntryDrawable(*entry));
    Prepare();
    EXPECT_EQ(preview.GetComponent<Components::MeshRenderer>(entity)->meshGpuHandleId, repaired.id);
    RenderGraph::RGFrame recovered(m_Device.get(), nullptr, nullptr, nullptr);
    EXPECT_TRUE(m_Services.Spine().BindWorldGeometry(recovered));
    recovered.DiscardRecordedPasses();
}

TEST_F(ClusterLODRuntimeTest, OwnerNamespaceReplacementClearsCopiedFineCacheAndRejectsOldGraph)
{
    Prepare();
    Prepare();
    ASSERT_NO_FATAL_FAILURE(ExpectFine());
    const auto copied = WithOrdinarySource(*m_World.GetComponent<Components::MeshRenderer>(m_Entity));
    RenderGraph::RGFrame old(m_Device.get(), nullptr, nullptr, nullptr);
    ASSERT_TRUE(m_Services.Spine().BindWorldGeometry(old));
    m_World.Clear();
    const auto replacement = m_World.Create().Set<Components::MeshRenderer>(copied)
        .Set<Components::WorldTransform>({}).GetHandle();
    m_World.ProcessCommands();
    m_Feature->Prepare(m_World);
    const auto* renderer = m_World.GetComponent<Components::MeshRenderer>(replacement);
    ASSERT_NE(renderer, nullptr);
    EXPECT_EQ(renderer->meshGpuHandleId, 0u);
    EXPECT_EQ(renderer->modelAssetGuid, copied.modelAssetGuid);
    EXPECT_EQ(renderer->materialAssetGuid, copied.materialAssetGuid);
    old.Execute();
    EXPECT_TRUE(old.Stats().ExecutionRejected);
    old.DiscardRecordedPasses();
}

TEST_F(ClusterLODRuntimeTest, OwnerDestructionClearsOrdinarySourceCloneAndRejectsOldGraph)
{
    auto owner = std::make_unique<ECS::World>();
    const auto entity = owner->Create()
        .Set<Components::MeshRenderer>(*m_World.GetComponent<Components::MeshRenderer>(m_Entity))
        .Set<Components::WorldTransform>({}).GetHandle();
    owner->ProcessCommands();
    m_Services.Views().SetViewWorldId(m_View, owner->GetWorldId());
    for (uint32 cycle = 0; cycle < 2; ++cycle)
    {
        m_Services.BeginWorldDrawFrame();
        m_Feature->Prepare(*owner);
    }
    const auto* selected = owner->GetComponent<Components::MeshRenderer>(entity);
    ASSERT_NE(selected, nullptr);
    const auto* fine = m_Services.GetMeshGPURegistry().Find(MeshGPUHandle(selected->meshGpuHandleId));
    ASSERT_NE(fine, nullptr);
    ASSERT_EQ(fine->StorageDomain, MeshGPUStorageDomain::ClusterFine);
    const auto copied = WithOrdinarySource(*selected);
    ECS::World preview;
    const auto clone = preview.Create().Set<Components::MeshRenderer>(copied)
        .Set<Components::WorldTransform>({}).GetHandle();
    preview.ProcessCommands();
    RenderGraph::RGFrame old(m_Device.get(), nullptr, nullptr, nullptr);
    ASSERT_TRUE(m_Services.Spine().BindWorldGeometry(old));
    owner.reset();
    m_Feature->Prepare(preview);
    const auto* renderer = preview.GetComponent<Components::MeshRenderer>(clone);
    ASSERT_NE(renderer, nullptr);
    EXPECT_EQ(renderer->meshGpuHandleId, 0u);
    EXPECT_EQ(renderer->modelAssetGuid, copied.modelAssetGuid);
    EXPECT_EQ(renderer->materialAssetGuid, copied.materialAssetGuid);
    old.Execute();
    EXPECT_TRUE(old.Stats().ExecutionRejected);
    old.DiscardRecordedPasses();
}

TEST_F(ClusterLODRuntimeTest, PendingForeignNamespaceReplacementIsRescannedDuringOwnerPrepare)
{
    Prepare();
    Prepare();
    ASSERT_NO_FATAL_FAILURE(ExpectFine());
    const auto copied = *m_World.GetComponent<Components::MeshRenderer>(m_Entity);
    ECS::World preview;
    preview.Create().Set<Components::MeshRenderer>(copied).Set<Components::WorldTransform>({}).SetEnabled(false);
    preview.ProcessCommands();
    const MeshGPUKey source{m_Model->GetGUID(), m_Submesh};
    ASSERT_TRUE(m_Services.GetMeshGPURegistry().UnregisterSubmesh(source));
    m_Services.BeginWorldDrawFrame();
    m_Feature->Prepare(preview);
    preview.Clear();
    const auto changed = WithOrdinarySource(copied);
    const auto replacement = preview.Create().Set<Components::MeshRenderer>(changed)
        .Set<Components::WorldTransform>({}).SetEnabled(false).GetHandle();
    preview.ProcessCommands();
    Prepare();
    const auto* renderer = preview.GetComponent<Components::MeshRenderer>(replacement);
    ASSERT_NE(renderer, nullptr);
    EXPECT_EQ(renderer->meshGpuHandleId, 0u);
    EXPECT_EQ(renderer->modelAssetGuid, changed.modelAssetGuid);
    EXPECT_EQ(renderer->materialAssetGuid, changed.materialAssetGuid);
}
} // namespace
} // namespace GameEngine::Engine::Renderer
