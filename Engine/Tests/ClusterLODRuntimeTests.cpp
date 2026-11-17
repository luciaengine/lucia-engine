#include "Engine/Rendering/ClusterLOD/ClusterLODFeature.h"
#include "Engine/Rendering/ClusterLOD/ClusterLODCut.h"

#include "Assets/AssetManager.h"
#include "Assets/ClusterLOD/MeshClusterLODAsset.h"
#include "Assets/MeshLODGenerator.h"
#include "ClusterLODTestMesh.h"
#include "Components/Rendering/DDGIVolume.h"
#include "Components/Hierarchy.h"
#include "Components/Rendering/LocalBounds.h"
#include "Components/Rendering/HLODVolume.h"
#include "Components/Rendering/MeshGPUData.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Transform.h"
#include "Components/Rendering/SkinnedMeshRenderer.h"
#include "Core/DebugMetrics.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/SystemScheduling.h"
#include "Engine/Rendering/ClusterLOD/ClusterLODPrepareSystem.h"
#include "Engine/Rendering/HlodRuntime.h"
#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/MeshLODThresholds.h"
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
#include <functional>
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
    ObserveMeshHandleSystem(std::function<uint64()> observe, uint64* handle) : m_Observe(std::move(observe)), m_Handle(handle) {}
    const char* GetName() const override { return "ObserveMeshHandle"; }
    void Update(ECS::World&, float32) override { *m_Handle = m_Observe(); }

  private:
    std::function<uint64()> m_Observe;
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
            GTEST_SKIP() << "No device available";
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
        config.MeshletsPerGroup = MeshletsPerGroup();
        String error;
        ASSERT_TRUE(CookMeshClusterLODAsset(source, path, config, error)) << error;
        m_Model = std::make_shared<ModelAsset>(GUID::Generate(), path);
        ASSERT_TRUE(m_Model->Load());
        m_Assets.RegisterLoadedAsset(m_Model->GetGUID(), m_Model);
        const auto manifest = m_Model->GetClusterLODManifest();
        ASSERT_NE(manifest, nullptr);
        ASSERT_GE(manifest->Meshes[0].ResidentDepth, 1u) << "the fixture must stream units under its resident cut";
        // A source unit: it draws only when the instance refines to the finest level.
        const auto unit = std::find_if(manifest->Units.begin(), manifest->Units.end(), [](const auto& candidate)
            { return candidate.IsStreamed() && candidate.Producer == kMeshClusterLODSourceGroup; });
        ASSERT_NE(unit, manifest->Units.end());
        m_Unit = static_cast<uint32>(unit - manifest->Units.begin());
        BeforeCoarseRegistration();
        const auto handles = m_Services.GetMeshGPURegistry().RegisterModelMeshes(m_Model->GetGUID(), *m_Model);
        AfterCoarseRegistration();
        ASSERT_EQ(handles.size(), 1u);
        m_Coarse = handles[0];
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
        renderer.meshId = 0;
        renderer.modelAssetGuid.Set(m_Model->GetGUID());
        renderer.materialAssetGuid.Set(m_MaterialGuid);
        renderer.meshGpuHandleId = m_Coarse.id;
        // A resolved world transform: parts copy it, so they may draw in their first frame.
        Components::WorldTransform transform{};
        transform.Version = 1;
        m_Entity = m_World.Create().Set<Components::MeshRenderer>(renderer)
            .Set<Components::WorldTransform>(transform).GetHandle();
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
    virtual uint32 MeshletsPerGroup() const { return 4; }
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

    // The part of m_Entity that draws the fixture's source unit; invalid while it has none.
    ECS::EntityHandle Part() { return PartOf(m_World, m_Entity); }

    ECS::EntityHandle PartOf(ECS::World& world, ECS::EntityHandle owner) const { return PartOf(world, owner, m_Unit); }

    ECS::EntityHandle PartOf(ECS::World& world, ECS::EntityHandle owner, uint32 unitIndex) const
    {
        // A coarser unit keeps its locked border, so it can have the same bounds: the index
        // count of the part's geometry tells them apart.
        const auto& unit = m_Model->GetClusterLODManifest()->Units[unitIndex];
        const auto bounds = Mathematics::BoundingBox::FromMinMax(unit.MinBounds, unit.MaxBounds);
        const auto& registry = m_Services.GetMeshGPURegistry();
        ECS::EntityHandle found;
        world.Query<ECS::Read<Components::Parent>, ECS::Read<Components::LocalBounds>,
                    ECS::Read<Components::MeshRenderer>>().Each(
            [owner, &bounds, &found, &registry, &unit](ECS::EntityHandle entity, const Components::Parent& parent,
                                                       const Components::LocalBounds& local,
                                                       const Components::MeshRenderer& renderer)
            {
                const auto* entry = registry.Find(MeshGPUHandle(renderer.meshGpuHandleId));
                if (parent.Entity == owner && local.Box.ToAABB() == bounds.ToAABB() &&
                    (!entry || entry->indexCount == unit.IndexCount))
                    found = entity;
            });
        return found;
    }

    static bool IsEvicted(const ECS::World& world, ECS::EntityHandle entity)
    {
        const auto* gpu = world.GetComponent<Components::MeshGPUData>(entity);
        return gpu && gpu->clusterEvicted;
    }

    // The fine geometry that draws the source unit of `owner`, or m_Coarse while it does not draw.
    MeshGPUHandle CurrentHandleOf(ECS::World& world, ECS::EntityHandle owner) const
    {
        const ECS::EntityHandle part = PartOf(world, owner);
        if (!part.IsValid() || IsEvicted(world, part))
            return m_Coarse;
        const MeshGPUHandle handle(world.GetComponent<Components::MeshRenderer>(part)->meshGpuHandleId);
        const auto* entry = m_Services.GetMeshGPURegistry().Find(handle);
        return entry && entry->StorageDomain == MeshGPUStorageDomain::ClusterFine ? handle : m_Coarse;
    }

    MeshGPUHandle CurrentHandle() { return CurrentHandleOf(m_World, m_Entity); }

    // Prepares frames until the source unit draws: each DAG level refines once its units can draw.
    void PrepareUntilFine()
    {
        ASSERT_TRUE(GameEngine::Tests::WaitUntil([this]
        {
            Prepare();
            return CurrentHandle() != m_Coarse;
        }, std::chrono::seconds(20)));
        ExpectFine();
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
    uint32 m_Unit = 0;
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
// Holds every submit on GatedQueue until Gate reaches 1.
class GatedQueueDevice final : public VulkanDevice
{
  public:
    SemaphoreHandle Gate;
    QueueType GatedQueue = QueueType::Graphics;
    bool QueueSubmit(QueueType queue, const std::vector<CommandList*>& commands,
                     const std::vector<std::pair<SemaphoreHandle, uint64_t>>& waits,
                     const std::vector<std::pair<SemaphoreHandle, uint64_t>>& signals) override
    {
        auto gated = waits;
        if (queue == GatedQueue && Gate.IsValid())
            gated.emplace_back(Gate, 1);
        return VulkanDevice::QueueSubmit(queue, commands, gated, signals);
    }
};

// Two gate semaphores, signalled by submits on the queue that the device does not gate.
class ScopedQueueGates
{
  public:
    explicit ScopedQueueGates(GatedQueueDevice& device)
        : Fine(device.CreateTimelineSemaphore()), Later(device.CreateTimelineSemaphore()), m_Device(device),
          m_ReleaseQueue(device.GatedQueue == IDevice::QueueType::Graphics ? IDevice::QueueType::Compute
                                                                           : IDevice::QueueType::Graphics) {}
    ~ScopedQueueGates()
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
            commands = m_Device.CreateCommandList(m_ReleaseQueue);
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
        m_FineReleased = Fine.IsValid() && m_Device.QueueSubmit(m_ReleaseQueue, {m_Signals[0].get()}, {}, {{Fine, 1}});
        return m_FineReleased;
    }
    bool ReleaseAll()
    {
        const bool fine = ReleaseFine();
        if (!m_LaterReleased)
            m_LaterReleased =
                Later.IsValid() && m_Device.QueueSubmit(m_ReleaseQueue, {m_Signals[1].get()}, {}, {{Later, 1}});
        if (!fine || !m_LaterReleased)
            return false;
        if (!m_Device.WaitTimelineSemaphoreValue(Fine, 1, kTimeoutNs) ||
            !m_Device.WaitTimelineSemaphoreValue(Later, 1, kTimeoutNs))
            return false;
        return std::all_of(std::begin(kQueues), std::end(kQueues), [this](IDevice::QueueType queue)
        {
            const auto last = m_Device.LastSubmissionToken(queue);
            return !last.IsValid() || m_Device.WaitGpuSyncToken(last, kTimeoutNs);
        });
    }

    const SemaphoreHandle Fine;
    const SemaphoreHandle Later;

  private:
    static constexpr IDevice::QueueType kQueues[] = {IDevice::QueueType::Graphics, IDevice::QueueType::Compute};
    static constexpr uint64 kTimeoutNs = 10'000'000'000ull;
    GatedQueueDevice& m_Device;
    const IDevice::QueueType m_ReleaseQueue;
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
        auto device = std::make_unique<GatedQueueDevice>();
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
    GatedQueueDevice* m_Gated = nullptr;
};

constexpr uint32 kFineReadBytes = 64;

// Copies kFineReadBytes of fine storage into `destination` in one pass on `queue`.
void SubmitFineCopy(RenderGraph::RGFrame& frame, RenderServices& services, BufferHandle source,
                    uint64 offset, BufferHandle destination,
                    RenderGraph::RGQueue queue = RenderGraph::RGQueue::Graphics)
{
    frame.BeginFrame(0);
    ASSERT_TRUE(services.Spine().BindWorldGeometry(frame));
    const auto src = frame.ImportExternalBuffer("Fine.Reader.Source", source, offset + kFineReadBytes);
    const auto dst = frame.ImportExternalBuffer("Fine.Reader.Destination", destination, kFineReadBytes);
    const auto setup = [src, dst](RenderGraph::RGPassBuilder& pass)
    {
        pass.Read(src, RenderGraph::RGBufferRead::CopySrc);
        pass.Write(dst, RenderGraph::RGBufferWrite::CopyDst);
    };
    const auto copy = [src, dst, offset](RenderGraph::RGContext& context)
    {
        context.Cmd->CopyBuffer(context.GetBuffer(src), context.GetBuffer(dst), kFineReadBytes, offset, 0);
    };
    if (queue == RenderGraph::RGQueue::Compute)
    {
        // A compute run with no independent graphics pass beside it moves to the graphics
        // queue (RGGraph::PlaceQueues).
        frame.AddComputePass("Fine.Reader.Copy", 0, setup, copy);
        frame.AddPass("Fine.Reader.Beside", 0, [](RenderGraph::RGPassBuilder& pass) { pass.PreventCulling(); },
                      [](RenderGraph::RGContext&) {});
    }
    else
    {
        frame.AddPass("Fine.Reader.Copy", 0, setup, copy);
    }
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
    ASSERT_NO_FATAL_FAILURE(PrepareUntilFine());
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
    ScopedQueueGates gates(*m_Gated);
    ASSERT_TRUE(gates.Initialize());
    ASSERT_TRUE(gates.Fine.IsValid());
    ASSERT_TRUE(gates.Later.IsValid());
    m_Gated->Gate = gates.Fine;
    ASSERT_NO_FATAL_FAILURE(SubmitFineCopy(first, m_Services, physical.coreVB, offset, readbacks[0]));
    ASSERT_NO_FATAL_FAILURE(SubmitFineCopy(second, m_Services, physical.coreVB, offset, readbacks[1]));
    const auto readers = m_Device->LastSubmissionToken(IDevice::QueueType::Graphics);
    ASSERT_TRUE(readers.IsValid());
    ASSERT_EQ(m_Device->QueryGpuSyncToken(readers), IDevice::GpuSyncStatus::Pending);
    ASSERT_FALSE(m_Device->LastSubmissionToken(IDevice::QueueType::Compute).IsValid())
        << "retirement takes its tokens before the first compute submit, so it must not wait for one";
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
    const auto newest = m_Device->LastSubmissionToken(IDevice::QueueType::Graphics);
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

// A frame that routes its compute pass to a second queue reads fine storage there. The
// graphics queue has completed everything, so only the compute token keeps the page.
TEST_F(ClusterLODGpuReaderTest, ComputeQueueReaderKeepsFineStorageUntilTheComputeTokenCompletes)
{
    if (!m_Device->GetValidationStats().Enabled)
        GTEST_SKIP() << "Vulkan validation layer unavailable for the real queue-lifetime witness";
    ASSERT_EQ(m_Device->GetValidationStats().ErrorCount, 0u);
    if (m_Device->GetComputeQueueFamilyIndex() == m_Device->GetGraphicsQueueFamilyIndex())
        GTEST_SKIP() << "a compute reader on its own queue requires a separate compute queue family";
    ASSERT_NO_FATAL_FAILURE(PrepareUntilFine());
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
    BufferDesc descriptor{};
    descriptor.size = kFineReadBytes;
    descriptor.usage = static_cast<uint32>(BufferUsage::TransferDst);
    descriptor.memoryUsage = BufferMemoryUsage::Readback;
    descriptor.flags = BufferCreateFlags::PersistentlyMapped;
    descriptor.debugName = "ClusterLOD.ComputeReader.Readback";
    const BufferHandle readback = m_Device->CreateBuffer(descriptor);
    ASSERT_TRUE(readback.IsValid());
    m_Device->WaitForIdle();
    const auto baselineDrains = m_Device->GetIdleDrainCount();
    ReaderFramePools pools(m_Device.get());
    RenderGraph::RGFrame reader(m_Device.get(), &pools.Persistent, &pools.Transient, &pools.Ring);
    reader.SetPhysicalQueueMap(RenderGraph::RGFrame::kIdentityQueueMap);
    m_Gated->GatedQueue = IDevice::QueueType::Compute;
    ScopedQueueGates gates(*m_Gated);
    ASSERT_TRUE(gates.Initialize());
    m_Gated->Gate = gates.Fine;
    ASSERT_NO_FATAL_FAILURE(
        SubmitFineCopy(reader, m_Services, physical.coreVB, offset, readback, RenderGraph::RGQueue::Compute));
    const auto computeReader = m_Device->LastSubmissionToken(IDevice::QueueType::Compute);
    ASSERT_TRUE(computeReader.IsValid()) << "the reader pass must run on the compute queue";
    ASSERT_EQ(m_Device->QueryGpuSyncToken(computeReader), IDevice::GpuSyncStatus::Pending);
    const auto graphics = m_Device->LastSubmissionToken(IDevice::QueueType::Graphics);
    ASSERT_TRUE(graphics.IsValid());
    ASSERT_NE(graphics.sem, computeReader.sem) << "the reader must run on its own queue timeline";
    ASSERT_TRUE(m_Device->WaitGpuSyncToken(graphics, 10'000'000'000ull));
    ASSERT_EQ(m_Device->QueryGpuSyncToken(computeReader), IDevice::GpuSyncStatus::Pending);
    m_Services.SetLODForceLevel(1);
    for (uint32 frame = 0; frame < 35; ++frame)
        Prepare();
    entry = m_Services.GetMeshGPURegistry().Find(fine);
    ASSERT_NE(entry, nullptr) << "the compute reader still waits on the owned GPU gate";
    ASSERT_TRUE(entry->DrawClosed);
    ASSERT_TRUE(gates.ReleaseFine());
    ASSERT_TRUE(GameEngine::Tests::WaitUntil([this, fine]
    {
        Prepare();
        return m_Services.GetMeshGPURegistry().Find(fine) == nullptr;
    }, std::chrono::seconds(10)));
    EXPECT_EQ(m_Device->QueryGpuSyncToken(computeReader), IDevice::GpuSyncStatus::Complete);
    EXPECT_EQ(m_Device->GetIdleDrainCount(), baselineDrains);
    const auto* copied = static_cast<const uint8*>(m_Device->MapBuffer(readback));
    ASSERT_NE(copied, nullptr);
    EXPECT_EQ(std::memcmp(copied, expected.data(), expected.size()), 0);
    m_Device->UnmapBuffer(readback);
    ASSERT_TRUE(gates.ReleaseAll());
    m_Device->DestroyBuffer(readback);
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
        m_Model->GetMesh(0), false, MeshGPUStorageDomain::ClusterFine);
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
    // The first frame activates the split; a later frame requests the pages.
    ASSERT_TRUE(GameEngine::Tests::WaitUntil([this]
    {
        Prepare();
        return m_Jobs->GetBackgroundQueuedForTests() > 0;
    }, std::chrono::seconds(10)))
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
    // A view whose eye is `origin + localEye`. The view matrix comes from the small local vectors,
    // and the exact eye goes in after it, so a far origin does not round the orientation.
    ViewId AddSseViewAt(const Mathematics::WorldPosition& origin, const Vector3& localEye, const Vector3& localCenter)
    {
        CameraData camera{};
        camera.proj = Mathematics::MakePerspectiveLH_ZO_ReverseZ(1.0471976f, 1.5f, 0.1f, 2'000'000.0f);
        camera.view = Mathematics::MakeLookAtLH(localEye, localCenter, Vector3(0, 1, 0));
        camera.viewProj = camera.proj * camera.view;
        SetCameraEye(camera, Mathematics::WorldPosition{origin.X + localEye.x, origin.Y + localEye.y,
                                                         origin.Z + localEye.z});
        auto& views = m_Services.Views();
        const auto cameraId = views.AllocateCamera("Cluster SSE far origin");
        views.SetCameraData(cameraId, camera, CameraRanges{0.1f, 2'000'000.0f});
        const auto id = views.AllocateView("Cluster SSE far origin", cameraId);
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
    const auto& manifest = *m_Model->GetClusterLODManifest();
    const auto& unit = manifest.Units[m_Unit];
    ASSERT_GT(manifest.Groups[unit.Group].Error, 0.0f);
    const Vector3 center = (unit.MinBounds + unit.MaxBounds) * 0.5f;
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
    ASSERT_NO_FATAL_FAILURE(PrepareUntilFine());
    ASSERT_TRUE(m_Services.Views().ReleaseView(nearView));
    Prepare();
    ASSERT_EQ(CurrentHandle(), m_Coarse);
    for (uint32 frame = 0; frame < 35; ++frame)
        Prepare();
    AddSseView(nearEye, center);
    ASSERT_NO_FATAL_FAILURE(PrepareUntilFine());
}

// The same near and far sequence, with the object and both cameras 100 km from the origin at a
// position that has a fraction. The selection must not depend on where the scene is.
TEST_P(ClusterLODMultipleViewTest, NearDemandAtALargeTranslationMatchesTheOrigin)
{
    ASSERT_TRUE(m_Services.Views().ReleaseView(m_View));
    m_Services.SetLODForceLevel(~0u);
    m_Services.GetMeshGPURegistry().SetLodSelectionMode(LodSelectionMode::Sse);
    m_Services.SetLODErrorBudgetPx(1.0f);
    const auto& manifest = *m_Model->GetClusterLODManifest();
    const auto& unit = manifest.Units[m_Unit];
    ASSERT_GT(manifest.Groups[unit.Group].Error, 0.0f);
    const Mathematics::WorldPosition origin{100000.03125, -60000.015625, 125000.0078125};
    m_World.GetComponentForWrite<Components::WorldTransform>(m_Entity)->Position = origin;
    const Vector3 center = (unit.MinBounds + unit.MaxBounds) * 0.5f;
    const Vector3 nearEye = center + Vector3(0, 2, -2);
    const Vector3 farEye = center + Vector3(0, 0, -1'000'000);
    const auto first = AddSseViewAt(origin, GetParam() ? nearEye : farEye, center);
    const auto second = AddSseViewAt(origin, GetParam() ? farEye : nearEye, center);
    const auto nearView = GetParam() ? first : second;
    m_Services.Views().SetViewRenderLayerMask(nearView, RenderLayerMask::None());
    Prepare();
    Prepare();
    ASSERT_EQ(CurrentHandle(), m_Coarse) << "the far view alone must select coarse";

    m_Services.Views().SetViewRenderLayerMask(nearView, RenderLayerMask::FromLayer(0));
    ASSERT_NO_FATAL_FAILURE(PrepareUntilFine());
    ASSERT_TRUE(m_Services.Views().ReleaseView(nearView));
    Prepare();
    ASSERT_EQ(CurrentHandle(), m_Coarse);
}

INSTANTIATE_TEST_SUITE_P(ViewRegistrationOrder, ClusterLODMultipleViewTest, ::testing::Bool());

// A one-mesh DAG that is a chain of groups: group k holds one unit made by group k - 1 (the
// source for group 0), with `triangles[k]` triangles. The last group is terminal.
MeshClusterLODManifest MakeChainManifest(std::initializer_list<uint32> triangles)
{
    MeshClusterLODManifest manifest;
    const auto count = static_cast<uint32>(triangles.size());
    manifest.Meshes.push_back({0, count, 0, count, count - 1});
    uint32 level = 0;
    for (const uint32 unitTriangles : triangles)
    {
        MeshClusterLODManifest::Group group;
        group.Depth = level;
        group.Error = static_cast<float>(level + 1);
        group.Terminal = level + 1 == count;
        group.FirstUnit = level;
        group.UnitCount = 1;
        group.FirstProduced = level;
        group.ProducedCount = group.Terminal ? 0 : 1;
        manifest.Groups.push_back(group);
        MeshClusterLODManifest::Unit unit;
        unit.Group = level;
        unit.Producer = level == 0 ? kMeshClusterLODSourceGroup : level - 1;
        unit.IndexCount = unitTriangles * 3;
        manifest.Units.push_back(unit);
        if (!group.Terminal)
            manifest.ProducedUnits.push_back(level + 1);
        ++level;
    }
    return manifest;
}

// The states of a chain whose groups are all ready, with the demands from the finest group up.
Vector<ClusterLODCutGroup> ReadyChain(std::initializer_list<double> demands)
{
    Vector<ClusterLODCutGroup> groups;
    for (const double demand : demands)
        groups.push_back({demand, false, true, true});
    return groups;
}

TEST(ClusterLODCut, RefinesTheLargestDemandFirstWithinTheTriangleBudget)
{
    const auto manifest = MakeChainManifest({8, 4, 2});
    auto first = ReadyChain({4.0, 8.0, 0.0});
    auto second = ReadyChain({1.5, 3.0, 0.0});
    Vector<ClusterLODCutDecision> firstCut(3);
    Vector<ClusterLODCutDecision> secondCut(3);
    const std::array<ClusterLODCutInstance, 2> instances{{{&manifest, 0, first, firstCut}, {&manifest, 0, second, secondCut}}};
    EXPECT_EQ(SelectClusterLODCuts(instances, 0, 0), 16u) << "without a cap both draw their source unit";
    EXPECT_TRUE(IsClusterLODUnitDrawn(manifest, 0, firstCut));
    EXPECT_TRUE(IsClusterLODUnitDrawn(manifest, 0, secondCut));

    // The first instance refines both its groups; the second instance's next step does not fit.
    EXPECT_EQ(SelectClusterLODCuts(instances, 10, 0), 10u);
    EXPECT_TRUE(IsClusterLODUnitDrawn(manifest, 0, firstCut));
    EXPECT_TRUE(IsClusterLODUnitDrawn(manifest, 2, secondCut));

    // One triangle less: the first source unit does not fit, a smaller step of the second does.
    EXPECT_EQ(SelectClusterLODCuts(instances, 9, 0), 8u);
    EXPECT_TRUE(IsClusterLODUnitDrawn(manifest, 1, firstCut));
    EXPECT_TRUE(IsClusterLODUnitDrawn(manifest, 1, secondCut));

    // The base triangles of unsplit instances count against the cap.
    EXPECT_EQ(SelectClusterLODCuts(instances, 10, 4), 10u);
    EXPECT_TRUE(IsClusterLODUnitDrawn(manifest, 1, firstCut));
    EXPECT_TRUE(IsClusterLODUnitDrawn(manifest, 2, secondCut));
}

TEST(ClusterLODCut, AGroupThatMustRefineIgnoresTheBudget)
{
    const auto manifest = MakeChainManifest({8, 4, 2});
    auto groups = ReadyChain({std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(), 0.0});
    Vector<ClusterLODCutDecision> cut(3);
    const ClusterLODCutInstance instance{&manifest, 0, groups, cut};
    EXPECT_EQ(SelectClusterLODCuts({&instance, 1}, 1, 0), 8u);
    EXPECT_TRUE(IsClusterLODUnitDrawn(manifest, 0, cut));
}

TEST(ClusterLODCut, AGroupRefinesOnlyWhenItsUnitsCanDraw)
{
    const auto manifest = MakeChainManifest({8, 4, 2});
    auto groups = ReadyChain({4.0, 8.0, 0.0});
    groups[0].UnitsReady = false;
    Vector<ClusterLODCutDecision> cut(3);
    const ClusterLODCutInstance instance{&manifest, 0, groups, cut};
    EXPECT_EQ(SelectClusterLODCuts({&instance, 1}, 0, 0), 4u);
    EXPECT_EQ(cut[0], ClusterLODCutDecision::RefineBlocked);
    EXPECT_TRUE(IsClusterLODUnitDrawn(manifest, 1, cut));
    EXPECT_FALSE(IsClusterLODUnitDrawn(manifest, 0, cut));
}

TEST(ClusterLODCut, ARefinedGroupStaysUntilItsProducedUnitsCanDraw)
{
    const auto manifest = MakeChainManifest({8, 4, 2});
    auto groups = ReadyChain({0.5, 8.0, 0.0});
    groups[0].WasRefined = true;
    groups[1].WasRefined = true;
    groups[0].ProducedReady = false;
    Vector<ClusterLODCutDecision> cut(3);
    const ClusterLODCutInstance instance{&manifest, 0, groups, cut};
    SelectClusterLODCuts({&instance, 1}, 0, 0);
    EXPECT_EQ(cut[0], ClusterLODCutDecision::CoarsenBlocked);
    EXPECT_TRUE(IsClusterLODUnitDrawn(manifest, 0, cut));
    groups[0].ProducedReady = true;
    SelectClusterLODCuts({&instance, 1}, 0, 0);
    EXPECT_EQ(cut[0], ClusterLODCutDecision::Coarse);
    EXPECT_TRUE(IsClusterLODUnitDrawn(manifest, 1, cut));
}

TEST(ClusterLODCut, ACoarseGroupHidesEveryGroupUnderIt)
{
    const auto manifest = MakeChainManifest({8, 4, 2});
    auto groups = ReadyChain({100.0, 0.5, 0.0});
    Vector<ClusterLODCutDecision> cut(3);
    const ClusterLODCutInstance instance{&manifest, 0, groups, cut};
    EXPECT_EQ(SelectClusterLODCuts({&instance, 1}, 0, 0), 2u);
    EXPECT_EQ(cut[0], ClusterLODCutDecision::Coarse) << "a group under a coarse group is not evaluated";
    EXPECT_TRUE(IsClusterLODUnitDrawn(manifest, 2, cut));
}

// Two instances of the fixture at one place, seen by one view at a pixel budget that every group
// exceeds. The runtime cut takes the scene-wide triangle budget.
class ClusterLODTriangleBudgetTest : public ClusterLODRuntimeTest
{
  protected:
    void SetUp() override
    {
        ClusterLODRuntimeTest::SetUp();
        if (IsSkipped() || HasFatalFailure())
            return;
        m_Other = m_World.Create().Set<Components::MeshRenderer>(*m_World.GetComponent<Components::MeshRenderer>(m_Entity))
            .Set<Components::WorldTransform>(*m_World.GetComponent<Components::WorldTransform>(m_Entity)).GetHandle();
        m_World.ProcessCommands();
        ASSERT_TRUE(m_Services.Views().ReleaseView(m_View));
        m_Services.SetLODForceLevel(~0u);
        m_Services.GetMeshGPURegistry().SetLodSelectionMode(LodSelectionMode::Sse);
        m_Services.SetLODErrorBudgetPx(kPixelBudget);
        const Mesh& coarse = m_Model->GetMesh(0);
        const Vector3 low{coarse.MinBounds[0], coarse.MinBounds[1], coarse.MinBounds[2]};
        const Vector3 high{coarse.MaxBounds[0], coarse.MaxBounds[1], coarse.MaxBounds[2]};
        const Vector3 center = (low + high) * 0.5f;
        const float extent = (high - low).Length();
        AddSseView(center + Vector3(0.0f, 2.0f * extent, -2.0f * extent), center);
    }

    bool IsFine(ECS::EntityHandle owner) { return CurrentHandleOf(m_World, owner) != m_Coarse; }

    void PrepareUntilBothFine()
    {
        ASSERT_TRUE(GameEngine::Tests::WaitUntil([this]
        {
            Prepare();
            return IsFine(m_Entity) && IsFine(m_Other);
        }, std::chrono::seconds(20)));
    }

    void PrepareFrames()
    {
        for (uint32 frame = 0; frame < 40; ++frame)
            Prepare();
    }

    // The resident cut of both instances: no group under it fits.
    uint64 ResidentTriangles() const { return 2 * m_Model->GetMesh(0).Indices.size() / 3; }

    static constexpr float kPixelBudget = 0.01f;
    ECS::EntityHandle m_Other;
};

TEST_F(ClusterLODTriangleBudgetTest, TheBudgetCapsEverySplitAndForcedLodZeroIgnoresIt)
{
    ASSERT_NO_FATAL_FAILURE(PrepareUntilBothFine()) << "without a cap both instances refine";

    m_Feature->SetTriangleBudget(ResidentTriangles());
    PrepareFrames();
    EXPECT_FALSE(IsFine(m_Entity));
    EXPECT_FALSE(IsFine(m_Other));

    m_Feature->SetTriangleBudget(0);
    ASSERT_NO_FATAL_FAILURE(PrepareUntilBothFine());

    m_Services.SetLODErrorBudgetPx(1'000'000.0f);
    PrepareFrames();
    EXPECT_FALSE(IsFine(m_Entity)) << "the pixel budget stays the floor";
    EXPECT_FALSE(IsFine(m_Other));

    m_Services.SetLODErrorBudgetPx(kPixelBudget);
    m_Feature->SetTriangleBudget(1);
    m_Services.SetLODForceLevel(0);
    ASSERT_NO_FATAL_FAILURE(PrepareUntilBothFine()) << "forced LOD 0 ignores the cap";
}

TEST_F(ClusterLODRuntimeTest, TheNextScheduledSystemSeesFineAndCoarseInTheSameCycle)
{
    uint64 observed = 0;
    ECS::SystemManager manager(m_Jobs.get());
    ECS::SystemScheduleBuilder schedule;
    schedule.Add<ClusterLODPrepareSystem>("ClusterLODPrepare", ECS::SystemPhase::Extraction, 0, {}, m_Feature);
    schedule.Add<ObserveMeshHandleSystem>("Observe", ECS::SystemPhase::Extraction, 1,
                                         {"ClusterLODPrepare"}, [this] { return CurrentHandle().id; }, &observed);
    schedule.BuildAndRegisterWithWaves(manager);
    m_Services.BeginWorldDrawFrame();
    manager.Update(m_World, 0.0f);
    EXPECT_EQ(observed, m_Coarse.id) << "pending fine IO must preserve the complete coarse cover";
    ASSERT_TRUE(GameEngine::Tests::WaitUntil([this, &manager]
    {
        m_Services.BeginWorldDrawFrame();
        manager.Update(m_World, 0.0f);
        return CurrentHandle() != m_Coarse;
    }, std::chrono::seconds(20)));
    ASSERT_NO_FATAL_FAILURE(ExpectFine());
    EXPECT_EQ(observed, CurrentHandle().id) << "no command flush may be needed between these systems";
    m_Services.SetLODForceLevel(1);
    m_Services.BeginWorldDrawFrame();
    manager.Update(m_World, 0.0f);
    EXPECT_EQ(CurrentHandle(), m_Coarse);
    EXPECT_EQ(observed, m_Coarse.id);
}

// A resident unit draws its range of the coarse mesh as its own registry entry. When that entry
// goes away, the split registers it again before the unit draws.
TEST_F(ClusterLODRuntimeTest, AResidentUnitRegistersAgainAfterItsEntryIsRemoved)
{
    ASSERT_NO_FATAL_FAILURE(PrepareUntilFine());
    const auto& manifest = *m_Model->GetClusterLODManifest();
    // A unit of a terminal group draws in the coarsest cut, which a view that wants nothing selects.
    const auto resident = std::find_if(manifest.Units.begin(), manifest.Units.end(), [&manifest](const auto& unit)
        { return !unit.IsStreamed() && manifest.Groups[unit.Group].Terminal; });
    ASSERT_NE(resident, manifest.Units.end());
    const auto unit = static_cast<uint32>(resident - manifest.Units.begin());
    const MeshGPUKey key{m_Model->GetGUID(), static_cast<uint32>(m_Model->GetMeshCount()) + unit};
    auto& registry = m_Services.GetMeshGPURegistry();
    const auto drawsUnit = [this, &registry, &key, unit]
    {
        Prepare();
        const auto part = PartOf(m_World, m_Entity, unit);
        return part.IsValid() && !IsEvicted(m_World, part) &&
            m_World.GetComponent<Components::MeshRenderer>(part)->meshGpuHandleId == registry.FindHandle(key).id;
    };
    // Away from forced LOD 0 the instance coarsens to its coarsest cut, which needs the unit.
    m_Services.SetLODForceLevel(1);
    ASSERT_TRUE(GameEngine::Tests::WaitUntil(drawsUnit, std::chrono::seconds(20)));
    const auto previous = registry.FindHandle(key);
    ASSERT_TRUE(previous.IsValid());
    m_Services.SetLODForceLevel(0);
    ASSERT_NO_FATAL_FAILURE(PrepareUntilFine());
    ASSERT_TRUE(registry.UnregisterSubmesh(key));
    m_Services.SetLODForceLevel(1);
    ASSERT_TRUE(GameEngine::Tests::WaitUntil(drawsUnit, std::chrono::seconds(20)));
    const auto refreshed = registry.FindHandle(key);
    EXPECT_NE(refreshed, previous);
    const auto* entry = registry.Find(refreshed);
    ASSERT_NE(entry, nullptr);
    EXPECT_TRUE(registry.IsEntryDrawable(*entry));
}

// Far away, a cluster instance is one entity with one GPU instance. Near, it draws one part per
// unit of its cut instead, and its own GPU instance is evicted, so the coarse mesh and the parts
// never draw together. The split ends when no view wants a streamed unit for the grace period.
TEST_F(ClusterLODRuntimeTest, ANearInstanceSplitsIntoThePartsOfItsCutAndEvictsItsOwner)
{
    const auto partCount = [this](bool drawn)
    {
        uint32 count = 0;
        m_World.Query<ECS::Read<Components::Parent>>().Each(
            [this, drawn, &count](ECS::EntityHandle entity, const Components::Parent& parent)
            { count += parent.Entity == m_Entity && (!drawn || !IsEvicted(m_World, entity)) ? 1u : 0u; });
        return count;
    };
    m_Services.SetLODForceLevel(1);
    Prepare();
    EXPECT_EQ(partCount(false), 0u);
    EXPECT_FALSE(IsEvicted(m_World, m_Entity));

    // The first cut is the resident cut: one part per unit of LOD0.
    const auto& manifest = *m_Model->GetClusterLODManifest();
    const auto residentUnits = std::count_if(manifest.Units.begin(), manifest.Units.end(), [&manifest](const auto& unit)
    {
        const auto& group = manifest.Groups[unit.Group];
        return IsMeshClusterLODUnitInLevel(group.Depth, group.Terminal, manifest.Meshes[0].ResidentDepth);
    });
    m_Services.SetLODForceLevel(0);
    Prepare();
    EXPECT_EQ(partCount(true), static_cast<uint32>(residentUnits));
    EXPECT_TRUE(IsEvicted(m_World, m_Entity));
    ASSERT_NO_FATAL_FAILURE(PrepareUntilFine());
    EXPECT_TRUE(IsEvicted(m_World, m_Entity));

    m_Services.SetLODForceLevel(1);
    for (uint32 frame = 0; frame < 35; ++frame)
        Prepare();
    EXPECT_EQ(partCount(false), 0u);
    EXPECT_FALSE(IsEvicted(m_World, m_Entity));
    EXPECT_EQ(CurrentHandle(), m_Coarse);
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
    ASSERT_NO_FATAL_FAILURE(PrepareUntilFine());
}

// HLOD replaces distant groups with a merged proxy; cluster LOD refines the members near the camera.
// Both run in one world: an HLOD volume and a runtime cluster do not turn fine pages off.
TEST_F(ClusterLODRuntimeTest, FinePagesStartInAWorldWithHlodClusters)
{
    m_World.Create().Set<Components::HLODVolume>({});
    m_World.ProcessCommands();
    auto& clusters = m_Services.GetHlodRuntime().GetClusters();
    clusters.push_back(Hlod::RuntimeCluster{});
    clusters.back().Members.push_back(m_Entity);
    ASSERT_NO_FATAL_FAILURE(PrepareUntilFine());
    clusters.clear();
}

// A member that HLOD replaced with its proxy draws nothing: it keeps the coarse cover, and
// its fine page loads once HLOD makes it resident again.
TEST_F(ClusterLODRuntimeTest, AnHlodEvictedMemberKeepsTheCoarseCover)
{
    Components::MeshGPUData evicted{};
    evicted.hlodEvicted = true;
    m_World.AddComponentImmediate(m_Entity, evicted);
    Prepare();
    Prepare();
    EXPECT_EQ(CurrentHandle(), m_Coarse);

    m_World.GetComponentForWrite<Components::MeshGPUData>(m_Entity)->hlodEvicted = false;
    ASSERT_NO_FATAL_FAILURE(PrepareUntilFine());

    // An eviction during a split ends it: the proxy draws, the parts do not.
    m_World.GetComponentForWrite<Components::MeshGPUData>(m_Entity)->hlodEvicted = true;
    Prepare();
    EXPECT_FALSE(Part().IsValid());
    EXPECT_EQ(CurrentHandle(), m_Coarse);
}

// HLOD compares the authored mesh of each member to detect a reassignment. The fine pages
// draw on generated parts, so the member keeps its authored mesh.
TEST_F(ClusterLODRuntimeTest, TheFineSwitchKeepsTheMembersHlodMeshSource)
{
    const auto* renderer = m_World.GetComponent<Components::MeshRenderer>(m_Entity);
    ASSERT_NE(renderer, nullptr);
    const Hlod::MemberMeshSource coarseSource = Hlod::MemberMeshSource::Of(*renderer);
    ASSERT_NO_FATAL_FAILURE(PrepareUntilFine());
    renderer = m_World.GetComponent<Components::MeshRenderer>(m_Entity);
    ASSERT_NE(renderer, nullptr);
    EXPECT_EQ(Hlod::MemberMeshSource::Of(*renderer), coarseSource);
}

TEST_F(ClusterLODRuntimeTest, MissingFineStoreKeepsCoarseCoverageAndFinishesTheRead)
{
    const auto store = ResolveMeshClusterLODStore(m_Model->GetPath(), {}, *m_Model->GetClusterLODManifest());
    const auto saved = store.string() + ".saved";
    std::filesystem::rename(store, saved);
    // The instance requests every streamed unit it wants, at most kMaximumPendingReads (8) per
    // frame, after its first frame activates the split.
    const auto& units = m_Model->GetClusterLODManifest()->Units;
    const auto streamed = std::count_if(units.begin(), units.end(), [](const auto& unit) { return unit.IsStreamed(); });
    for (int64 frame = 0; frame < (streamed + 7) / 8 + 3; ++frame)
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
    // A failed read retries after 120 frames.
    for (uint32 frame = 0; frame < 120; ++frame)
        Prepare();
    ASSERT_NO_FATAL_FAILURE(PrepareUntilFine());
}

TEST_F(ClusterLODRuntimeTest, LiveOpaqueToBlendAndBackChangesOnlyTheCachedGeometry)
{
    ASSERT_NO_FATAL_FAILURE(PrepareUntilFine());
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
    ASSERT_NO_FATAL_FAILURE(PrepareUntilFine());
    EXPECT_EQ(m_World.GetComponent<Components::MeshRenderer>(m_Entity)->materialAssetGuid.ToGuid(), m_MaterialGuid);
}

TEST_F(ClusterLODRuntimeTest, ANewSkinComponentRestoresCoarseBeforeTheNextExtraction)
{
    ASSERT_NO_FATAL_FAILURE(PrepareUntilFine());
    m_World.AddComponent(m_Entity, Components::SkinnedMeshRenderer{});
    m_World.ProcessCommands();
    Prepare();
    EXPECT_EQ(CurrentHandle(), m_Coarse);
}

TEST_F(ClusterLODRuntimeTest, AnAbandonedArenaReleasesDecodedOwnershipAndCannotReplayAReusedRow)
{
    ASSERT_NO_FATAL_FAILURE(PrepareUntilFine());
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
    ASSERT_NO_FATAL_FAILURE(PrepareUntilFine());
    // Every fine page of the instance frees and reloads, so the old row can go to another part.
    for (uint32 frame = 0; frame < 20; ++frame)
        Prepare();
    bool reused = false;
    m_World.Query<ECS::Read<Components::Parent>, ECS::Read<Components::MeshRenderer>>().Each(
        [this, oldRow, &reused](ECS::EntityHandle, const Components::Parent& parent,
                                const Components::MeshRenderer& renderer)
        {
            const auto* entry = m_Services.GetMeshGPURegistry().Find(MeshGPUHandle(renderer.meshGpuHandleId));
            reused |= parent.Entity == m_Entity && entry && entry->StorageDomain == MeshGPUStorageDomain::ClusterFine &&
                entry->gpuMeshIndex == oldRow;
        });
    EXPECT_TRUE(reused) << "the witness must exercise numeric row reuse";
    abandoned.Execute();
    EXPECT_TRUE(abandoned.Stats().ExecutionRejected);
    EXPECT_EQ(recorded, 0u);
    abandoned.DiscardRecordedPasses();
}

TEST_F(ClusterLODRuntimeTest, AnOrdinarySecondWorldResealsItsOwnAppEpoch)
{
    ASSERT_NO_FATAL_FAILURE(PrepareUntilFine());
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
    ASSERT_NO_FATAL_FAILURE(PrepareUntilFine());
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
    ASSERT_NO_FATAL_FAILURE(PrepareUntilFine());
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
    ASSERT_NO_FATAL_FAILURE(PrepareUntilFine());
    m_World.Create().Set<Components::DDGIVolume>({}).Set<Components::WorldTransform>({});
    m_World.ProcessCommands();
    Prepare();
    EXPECT_EQ(CurrentHandle(), m_Coarse);
}

TEST_F(ClusterLODRuntimeTest, DisabledOwnerWorldCloneUnbindsItsCopiedFineCache)
{
    ASSERT_NO_FATAL_FAILURE(PrepareUntilFine());
    const auto fine = CurrentHandle();
    const auto clone = ECS::Entity(&m_World, Part()).Clone().SetEnabled(false).GetHandle();
    m_World.ProcessCommands();
    // A root copy: the fixture finds the part by its parent.
    m_World.RemoveComponentImmediate<Components::Parent>(clone);
    const auto* renderer = m_World.GetComponent<Components::MeshRenderer>(clone);
    ASSERT_NE(renderer, nullptr);
    ASSERT_EQ(renderer->meshGpuHandleId, fine.id);
    Prepare();
    EXPECT_EQ(m_World.GetComponent<Components::MeshRenderer>(clone)->meshGpuHandleId, MeshGPUHandle{}.id);
    EXPECT_EQ(CurrentHandle(), fine) << "the active original still requests fine geometry";
}

TEST_F(ClusterLODRuntimeTest, OwnerWorldCloneWithOrdinarySourceClearsItsCopiedFineCache)
{
    ASSERT_NO_FATAL_FAILURE(PrepareUntilFine());
    const auto fine = CurrentHandle();
    const auto clone = ECS::Entity(&m_World, Part()).Clone().GetHandle();
    m_World.ProcessCommands();
    // A root copy: the fixture finds the part by its parent.
    m_World.RemoveComponentImmediate<Components::Parent>(clone);
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

TEST_F(ClusterLODRuntimeTest, SecondClusterWorldUnbindsCopiedFineCachesIncludingDisabledEntities)
{
    ASSERT_NO_FATAL_FAILURE(PrepareUntilFine());
    const auto copied = *m_World.GetComponent<Components::MeshRenderer>(Part());
    ECS::World preview;
    const auto active = preview.Create().Set<Components::MeshRenderer>(copied)
        .Set<Components::WorldTransform>({}).GetHandle();
    const auto disabled = preview.Create().Set<Components::MeshRenderer>(copied)
        .Set<Components::WorldTransform>({}).SetEnabled(false).GetHandle();
    preview.ProcessCommands();
    m_Services.BeginWorldDrawFrame();
    m_Feature->Prepare(preview);
    EXPECT_EQ(CurrentHandle(), m_Coarse);
    EXPECT_EQ(preview.GetComponent<Components::MeshRenderer>(active)->meshGpuHandleId, MeshGPUHandle{}.id);
    EXPECT_EQ(preview.GetComponent<Components::MeshRenderer>(disabled)->meshGpuHandleId, MeshGPUHandle{}.id);
    RenderGraph::RGFrame frame(m_Device.get(), nullptr, nullptr, nullptr);
    EXPECT_TRUE(m_Services.Spine().BindWorldGeometry(frame));
    frame.DiscardRecordedPasses();
}

TEST_F(ClusterLODRuntimeTest, LateFineInstanceInvalidatesAnAlreadySealedCoarseCut)
{
    ASSERT_NO_FATAL_FAILURE(PrepareUntilFine());
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
    ASSERT_NO_FATAL_FAILURE(PrepareUntilFine());
    const auto copied = *m_World.GetComponent<Components::MeshRenderer>(Part());
    ECS::World preview;
    const auto entity = preview.Create().Set<Components::MeshRenderer>(copied)
        .Set<Components::WorldTransform>({}).SetEnabled(false).GetHandle();
    preview.ProcessCommands();
    m_Services.BeginWorldDrawFrame();
    m_Feature->Prepare(preview);
    EXPECT_EQ(CurrentHandle(), m_Coarse);
    EXPECT_EQ(preview.GetComponent<Components::MeshRenderer>(entity)->meshGpuHandleId, MeshGPUHandle{}.id);
}

TEST_F(ClusterLODRuntimeTest, ForeignFineCacheWithChangedSourceIsClearedBeforeExtraction)
{
    ASSERT_NO_FATAL_FAILURE(PrepareUntilFine());
    const auto copied = WithOrdinarySource(*m_World.GetComponent<Components::MeshRenderer>(Part()));
    ECS::World preview;
    const auto entity = preview.Create().Set<Components::MeshRenderer>(copied)
        .Set<Components::WorldTransform>({}).GetHandle();
    preview.ProcessCommands();
    m_Services.BeginWorldDrawFrame();
    m_Feature->Prepare(preview);
    EXPECT_EQ(CurrentHandle(), m_Coarse);
    EXPECT_EQ(preview.GetComponent<Components::MeshRenderer>(entity)->meshGpuHandleId, 0u);
}

TEST_F(ClusterLODRuntimeTest, OwnerNamespaceReplacementClearsCopiedFineCacheAndRejectsOldGraph)
{
    ASSERT_NO_FATAL_FAILURE(PrepareUntilFine());
    const auto copied = WithOrdinarySource(*m_World.GetComponent<Components::MeshRenderer>(Part()));
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
        .Set<Components::WorldTransform>(*m_World.GetComponent<Components::WorldTransform>(m_Entity)).GetHandle();
    owner->ProcessCommands();
    m_Services.Views().SetViewWorldId(m_View, owner->GetWorldId());
    ASSERT_TRUE(GameEngine::Tests::WaitUntil([this, &owner, entity]
    {
        m_Services.BeginWorldDrawFrame();
        m_Feature->Prepare(*owner);
        return CurrentHandleOf(*owner, entity) != m_Coarse;
    }, std::chrono::seconds(20)));
    const auto* selected = owner->GetComponent<Components::MeshRenderer>(PartOf(*owner, entity));
    ASSERT_NE(selected, nullptr);
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

} // namespace
} // namespace GameEngine::Engine::Renderer
