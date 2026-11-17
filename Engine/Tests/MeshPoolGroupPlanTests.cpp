// Tests for MeshPoolGroupPlan: geometry-bind identity grouping for opaque draw
// consolidation. Meshes sharing {vertex-flags bucket, stream pools, index pool,
// index type, topology} must share a group id; any bind-relevant difference
// must split; ids must be append-only across Refresh calls (the consumer-vs-
// snapshot stability contract).

#include <gtest/gtest.h>

#include "Engine/Rendering/MeshPoolGroupPlan.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "Assets/ModelAsset.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/GPUDrawStreamBuilder.h"
#include "Rendering/Core/GPUScene.h"

#include "TestDeviceHelper.h"
#include "../Modules/Rendering/Tests/ScopedEnvVar.h"
#if defined(RENDERING_HAS_VULKAN)
#include "Source/Vulkan/VulkanDevice/VulkanDevice.h"
#endif

#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;

namespace
{

class MeshPoolGroupPlanTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Device = CreateDevice();
        if (!m_Device)
            GTEST_SKIP() << "No Vulkan device available";
        m_Scene = std::make_unique<GPUScene>(m_Device.get());
        ASSERT_TRUE(m_Scene->Initialize(64u, 16u));
        m_Registry.Initialize(m_Device.get());
        m_Registry.SetGPUScene(m_Scene.get());
    }

    virtual std::unique_ptr<IDevice> CreateDevice() { return CreateVulkanDeviceFast(); }

    void TearDown() override
    {
        m_Registry.Shutdown();
        if (m_Scene)
            m_Scene->Shutdown();
        m_Scene.reset();
        if (m_Device)
            m_Device->Shutdown();
    }

    // Minimal triangle mesh; the optional streams control the registry bucket.
    Mesh MakeTriangle(bool withColor, bool wideIndices)
    {
        Mesh m{};
        m.Name = "PlanTriangle";
        Vertex v0{}, v1{}, v2{};
        v0.Position[1] = 1.0f;
        v1.Position[0] = -1.0f;
        v1.Position[1] = -1.0f;
        v2.Position[0] = 1.0f;
        v2.Position[1] = -1.0f;
        v0.Normal[2] = v1.Normal[2] = v2.Normal[2] = 1.0f;
        m.Vertices = {v0, v1, v2};
        m.Indices = {0u, 1u, 2u};
        if (wideIndices)
        {
            // Force a vertex count past 65535 so the upload picks uint32
            // indices (a distinct index pool + index type).
            m.Vertices.resize(70000, v2);
            m.Indices = {0u, 1u, 69999u};
        }
        if (withColor)
            m.Color0.resize(m.Vertices.size() * 4, 1.0f);
        m.MinBounds[0] = -1.0f; m.MinBounds[1] = -1.0f; m.MinBounds[2] = 0.0f;
        m.MaxBounds[0] = 1.0f;  m.MaxBounds[1] = 1.0f;  m.MaxBounds[2] = 0.0f;
        return m;
    }

    uint32_t RegisterMesh(uint32_t submesh, const Mesh& mesh)
    {
        MeshGPUKey key{};
        key.assetGuid = m_Guid;
        key.submeshIndex = submesh;
        const MeshGPUHandle handle = m_Registry.RegisterSubmesh(key, mesh);
        const MeshGPUEntry* entry = m_Registry.Find(handle);
        EXPECT_NE(entry, nullptr);
        return entry ? entry->gpuMeshIndex : ~0u;
    }

    uint32_t MeshTableCount() const
    {
        return static_cast<uint32_t>(m_Scene->GetMeshes().size());
    }

    bool HasCoherentDeviceLocalUploadMemory() const
    {
        BufferDesc desc{};
        desc.size = 4096;
        desc.usage = static_cast<uint32_t>(BufferUsage::Vertex);
        desc.memoryUsage = BufferMemoryUsage::UploadDeviceLocalPreferred;
        desc.flags = BufferCreateFlags::PersistentlyMapped;
        const auto buffer = m_Device->CreateBuffer(desc);
        if (!buffer.IsValid())
            return false;
        const auto memory = m_Device->GetBufferMemoryResidency(buffer);
        m_Device->DestroyBuffer(buffer);
        return memory.reported && memory.deviceLocal && memory.hostVisible && memory.hostCoherent;
    }

    std::unique_ptr<IDevice> m_Device;
    std::unique_ptr<GPUScene> m_Scene;
    MeshGPURegistry m_Registry;
    GUID m_Guid = GUID::Generate();
};

class FinePoolRecoveryTest : public MeshPoolGroupPlanTest
{
  protected:
    void SetUp() override
    {
        MeshPoolGroupPlanTest::SetUp();
        if (HasFatalFailure() || IsSkipped())
            return;
        if (!HasCoherentDeviceLocalUploadMemory())
            GTEST_SKIP() << "fine domain requires reported coherent host-visible device-local upload memory";
        if (m_Device->GetAPI() != GraphicsAPI::Vulkan)
            GTEST_SKIP() << "the forced device loss (GE_VK_FORCE_DEVICE_LOST) exists only on Vulkan";
        m_Device->RegisterDeviceRebuiltCallback("FinePoolRecoveryTest", [this](IDevice* device)
        {
            m_Scene->ReprovisionAfterDeviceRebuild();
            m_Registry.ReprovisionAfterDeviceRebuild([this](const MeshGPUKey& key)
            {
                return key == m_RebuildKey ? MeshGPUCpuSource{&m_RebuildSource, MeshGPUSourceOrigin::Asset}
                                          : MeshGPUCpuSource{};
            });
            device->NotifyReprovisionComplete();
        });
    }

    void RecoverTo(uint64_t generation)
    {
        for (uint32_t attempt = 0; attempt < 32 && m_Device->GetDeviceRebuildGeneration() < generation; ++attempt)
        {
            m_Device->TickDeviceRecovery();
            if (!m_Device->BeginFrame())
                continue;
            auto commands = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
            ASSERT_NE(commands, nullptr);
            commands->Begin();
            commands->End();
            m_Device->ExecuteCommandLists({commands.get()});
            m_Device->Present();
        }
        ASSERT_EQ(m_Device->GetDeviceRebuildGeneration(), generation);
    }

    GameEngine::Rendering::Tests::ScopedEnvVar m_ForceLoss{"GE_VK_FORCE_DEVICE_LOST", "1,3"};
    MeshGPUKey m_RebuildKey{m_Guid, 0};
    Mesh m_RebuildSource;
};

TEST_F(FinePoolRecoveryTest, PreservesFineIdentityAndRefusesChangedRebuildGeometry)
{
    const Mesh mesh = MakeTriangle(false, false);
    m_RebuildSource = mesh;
    const auto handle = m_Registry.RegisterSubmesh(m_RebuildKey, mesh, false, MeshGPUStorageDomain::ClusterFine);
    ASSERT_NE(m_Registry.Find(handle), nullptr);
    ASSERT_GT(m_Registry.Find(handle)->indexCount, 0u);
    const auto originalIdentity = m_Registry.Find(handle)->FineSourceContentHash;
    ASSERT_NO_FATAL_FAILURE(RecoverTo(1));
    ASSERT_FALSE(HasFatalFailure());
    const auto* restored = m_Registry.Find(handle);
    ASSERT_NE(restored, nullptr);
    EXPECT_EQ(restored->StorageDomain, MeshGPUStorageDomain::ClusterFine);
    ASSERT_GT(restored->indexCount, 0u);
    EXPECT_EQ(restored->contentHash, originalIdentity);
    const auto upload = restored->uploadSeq;
    EXPECT_EQ(m_Registry.RegisterSubmesh(m_RebuildKey, mesh, false, MeshGPUStorageDomain::ClusterFine), handle);
    EXPECT_EQ(m_Registry.Find(handle)->uploadSeq, upload);

    m_RebuildSource.Vertices.front().Position[0] = 0.25f;
    ASSERT_NO_FATAL_FAILURE(RecoverTo(2));
    ASSERT_FALSE(HasFatalFailure());
    ASSERT_NE(m_Registry.Find(handle), nullptr);
    EXPECT_EQ(m_Registry.Find(handle)->indexCount, 0u);
    EXPECT_EQ(m_Registry.Find(handle)->contentHash, 0u);
    EXPECT_EQ(m_Registry.Find(handle)->FineSourceContentHash, originalIdentity);
    EXPECT_FALSE(m_Registry.RegisterSubmesh(m_RebuildKey, m_RebuildSource, false,
                                            MeshGPUStorageDomain::ClusterFine).IsValid());
    EXPECT_EQ(m_Registry.RegisterSubmesh(m_RebuildKey, mesh, false, MeshGPUStorageDomain::ClusterFine), handle);
    EXPECT_GT(m_Registry.Find(handle)->indexCount, 0u);
    EXPECT_EQ(m_Registry.Find(handle)->contentHash, originalIdentity);
}

TEST_F(FinePoolRecoveryTest, SuccessfulReprovisionOfFailedFineUploadBecomesAnImmutableDedupHit)
{
    Mesh mesh = MakeTriangle(false, false);
    mesh.Vertices.resize((16ull << 20) / (8 * sizeof(float)));
    const MeshGPUKey firstKey{m_Guid, 1};
    const auto first = m_Registry.RegisterSubmesh(firstKey, mesh, false, MeshGPUStorageDomain::ClusterFine);
    ASSERT_NE(m_Registry.Find(first), nullptr);
    ASSERT_GT(m_Registry.Find(first)->indexCount, 0u);
    const auto failed = m_Registry.RegisterSubmesh(m_RebuildKey, mesh, false, MeshGPUStorageDomain::ClusterFine);
    ASSERT_NE(m_Registry.Find(failed), nullptr);
    ASSERT_EQ(m_Registry.Find(failed)->indexCount, 0u);
    ASSERT_EQ(m_Registry.Find(failed)->contentHash, 0u);
    const auto identity = m_Registry.Find(failed)->FineSourceContentHash;
    m_RebuildSource = mesh;
    ASSERT_NO_FATAL_FAILURE(RecoverTo(1));
    ASSERT_FALSE(HasFatalFailure());
    EXPECT_EQ(m_Registry.Find(first)->indexCount, 0u);
    ASSERT_GT(m_Registry.Find(failed)->indexCount, 0u);
    EXPECT_EQ(m_Registry.Find(failed)->contentHash, identity);
    const auto upload = m_Registry.Find(failed)->uploadSeq;
    EXPECT_EQ(m_Registry.RegisterSubmesh(m_RebuildKey, mesh, false, MeshGPUStorageDomain::ClusterFine), failed);
    EXPECT_EQ(m_Registry.Find(failed)->uploadSeq, upload);
}

TEST_F(FinePoolRecoveryTest, ClosedFineLookupStaysClosedAfterRebuildAndTombstoning)
{
    const Mesh mesh = MakeTriangle(false, false);
    m_RebuildSource = mesh;
    const auto fine = m_Registry.RegisterSubmesh(m_RebuildKey, mesh, false, MeshGPUStorageDomain::ClusterFine);
    ASSERT_NE(m_Registry.Find(fine), nullptr);
    ASSERT_TRUE(m_Registry.IsEntryDrawable(*m_Registry.Find(fine)));
    ASSERT_TRUE(m_Registry.CloseFineDraws(fine));
    ASSERT_NO_FATAL_FAILURE(RecoverTo(1));
    ASSERT_NE(m_Registry.Find(fine), nullptr);
    EXPECT_TRUE(m_Registry.Find(fine)->DrawClosed);
    EXPECT_FALSE(m_Registry.IsEntryDrawable(*m_Registry.Find(fine)));
    EXPECT_FALSE(m_Registry.RegisterSubmesh(m_RebuildKey, mesh, false, MeshGPUStorageDomain::ClusterFine).IsValid());
    m_RebuildSource.Vertices.front().Position[0] += 0.125f;
    ASSERT_NO_FATAL_FAILURE(RecoverTo(2));
    ASSERT_NE(m_Registry.Find(fine), nullptr);
    EXPECT_EQ(m_Registry.Find(fine)->contentHash, 0u);
    EXPECT_TRUE(m_Registry.Find(fine)->DrawClosed);
    EXPECT_FALSE(m_Registry.RegisterSubmesh(m_RebuildKey, mesh, false, MeshGPUStorageDomain::ClusterFine).IsValid());
}

#if defined(RENDERING_HAS_VULKAN)
class RefusingFineWriteDevice final : public VulkanDevice
{
  public:
    uint32_t Writes = 0;
    uint32_t RefuseAt = 0;
    bool TryWriteCoherentBuffer(BufferHandle handle, size_t offset, size_t size, const void* data) override
    {
        ++Writes;
        if (RefuseAt != 0 && Writes == RefuseAt)
            return false;
        return VulkanDevice::TryWriteCoherentBuffer(handle, offset, size, data);
    }
};

class FineWriteFailureTest : public MeshPoolGroupPlanTest
{
  protected:
    std::unique_ptr<IDevice> CreateDevice() override
    {
        DeviceDesc desc{};
        desc.preferredAPI = GraphicsAPI::Vulkan;
        desc.enableDynamicRendering = true;
        desc.enableSwapchain = false;
        auto device = std::make_unique<RefusingFineWriteDevice>();
        if (!device->Initialize(desc))
            return {};
        m_Writer = device.get();
        return device;
    }
    RefusingFineWriteDevice* m_Writer = nullptr;
};

TEST_F(FineWriteFailureTest, LaterWriteRefusalKeepsOldGeometryAndAllowsSameKeyRetry)
{
    if (!HasCoherentDeviceLocalUploadMemory())
        GTEST_SKIP() << "fine domain requires reported coherent host-visible device-local upload memory";
    const auto mesh = MakeTriangle(false, false);
    const auto ordinary = RegisterMesh(0, mesh);
    const auto sharedCapacity = m_Registry.GetStorageDomainCapacityBytes(MeshGPUStorageDomain::Shared);
    const MeshGPUKey key{m_Guid, 1};
    m_Writer->RefuseAt = 2; // core copy succeeds; index copy refuses before writing
    const auto failed = m_Registry.RegisterSubmesh(key, mesh, true, MeshGPUStorageDomain::ClusterFine);
    ASSERT_NE(m_Registry.Find(failed), nullptr);
    EXPECT_EQ(m_Writer->Writes, 2u);
    EXPECT_EQ(m_Registry.Find(failed)->indexCount, 0u);
    EXPECT_FALSE(m_Registry.IsEntryDrawable(*m_Registry.Find(failed)));
    EXPECT_EQ(m_Registry.GetStorageDomainCapacityBytes(MeshGPUStorageDomain::ClusterFine), 0u);
    EXPECT_EQ(m_Registry.GetStorageDomainCapacityBytes(MeshGPUStorageDomain::Shared), sharedCapacity);
    m_Writer->RefuseAt = 0;
    const auto retried = m_Registry.RegisterSubmesh(key, mesh, true, MeshGPUStorageDomain::ClusterFine);
    EXPECT_EQ(retried, failed);
    ASSERT_NE(m_Registry.Find(retried), nullptr);
    EXPECT_TRUE(m_Registry.IsEntryDrawable(*m_Registry.Find(retried)));
    MeshPoolGroupPlan plan;
    plan.Refresh(m_Registry, MeshTableCount());
    EXPECT_NE(plan.GroupOf(ordinary), MeshPoolGroupPlan::kAbsentGroup);
    EXPECT_NE(plan.GroupOf(m_Registry.Find(retried)->gpuMeshIndex), MeshPoolGroupPlan::kAbsentGroup);
}
TEST_F(FineWriteFailureTest, AClosedFailedUploadCannotAllocateAgainUnderTheSameKey)
{
    if (!HasCoherentDeviceLocalUploadMemory())
        GTEST_SKIP() << "fine domain requires reported coherent host-visible device-local upload memory";
    const auto mesh = MakeTriangle(false, false);
    const MeshGPUKey key{m_Guid, 1};
    m_Writer->RefuseAt = 1;
    const auto failed = m_Registry.RegisterSubmesh(key, mesh, false, MeshGPUStorageDomain::ClusterFine);
    ASSERT_NE(m_Registry.Find(failed), nullptr);
    ASSERT_EQ(m_Registry.Find(failed)->contentHash, 0u);
    ASSERT_TRUE(m_Registry.CloseFineDraws(failed));
    m_Writer->RefuseAt = 0;
    const auto attempts = m_Writer->Writes;
    EXPECT_FALSE(m_Registry.RegisterSubmesh(key, mesh, false, MeshGPUStorageDomain::ClusterFine).IsValid());
    EXPECT_EQ(m_Writer->Writes, attempts) << "closure must refuse before any physical upload";
    EXPECT_EQ(m_Registry.GetStorageDomainCapacityBytes(MeshGPUStorageDomain::ClusterFine), 0u);
    EXPECT_FALSE(m_Registry.IsEntryDrawable(*m_Registry.Find(failed)));
}

#endif

TEST_F(MeshPoolGroupPlanTest, SameBucketAndPoolsShareOneGroup)
{
    const uint32_t meshA = RegisterMesh(0, MakeTriangle(false, false));
    const uint32_t meshB = RegisterMesh(1, MakeTriangle(false, false));
    ASSERT_NE(meshA, ~0u);
    ASSERT_NE(meshB, ~0u);

    MeshPoolGroupPlan plan;
    plan.Refresh(m_Registry, MeshTableCount());

    EXPECT_EQ(plan.GroupOf(meshA), plan.GroupOf(meshB));
    EXPECT_NE(plan.GroupOf(meshA), MeshPoolGroupPlan::kAbsentGroup);
    EXPECT_EQ(plan.LiveGroupCount(), 1u);
}

TEST_F(MeshPoolGroupPlanTest, PhysicalDomainsSplitPoolsAndGroupsWithoutChangingShaderFlags)
{
    if (!HasCoherentDeviceLocalUploadMemory())
        GTEST_SKIP() << "fine domain requires reported coherent host-visible device-local upload memory";
    const auto mesh = MakeTriangle(false, false);
    const auto shared = m_Registry.RegisterSubmesh({m_Guid, 0}, mesh);
    const MeshGPUKey fineKey{m_Guid, 1};
    const auto fine = m_Registry.RegisterSubmesh(fineKey, mesh, true, MeshGPUStorageDomain::ClusterFine);
    const auto* sharedEntry = m_Registry.Find(shared);
    const auto* fineEntry = m_Registry.Find(fine);
    ASSERT_NE(sharedEntry, nullptr);
    ASSERT_NE(fineEntry, nullptr);
    ASSERT_GT(sharedEntry->indexCount, 0u);
    ASSERT_GT(fineEntry->indexCount, 0u);
    EXPECT_EQ(sharedEntry->vertexFlags, fineEntry->vertexFlags);
    EXPECT_EQ(sharedEntry->bucketKey, fineEntry->bucketKey);
    EXPECT_NE(sharedEntry->StorageDomain, fineEntry->StorageDomain);
    EXPECT_NE(sharedEntry->gpuMeshIndex, fineEntry->gpuMeshIndex);
    MeshGPUEntryBindings sharedBindings, fineBindings;
    ASSERT_TRUE(m_Registry.TryGetDrawableBindings(*sharedEntry, sharedBindings));
    ASSERT_TRUE(m_Registry.TryGetDrawableBindings(*fineEntry, fineBindings));
    EXPECT_NE(sharedBindings, fineBindings);
    MeshPoolGroupPlan plan;
    plan.Refresh(m_Registry, MeshTableCount());
    EXPECT_NE(plan.GroupOf(sharedEntry->gpuMeshIndex), plan.GroupOf(fineEntry->gpuMeshIndex));

    Mesh changed = mesh;
    changed.Vertices.front().Position[0] = 0.25f;
    EXPECT_FALSE(m_Registry.RegisterSubmesh(fineKey, changed, true,
                                           MeshGPUStorageDomain::ClusterFine).IsValid());
    EXPECT_FALSE(m_Registry.RegisterSubmesh(fineKey, mesh).IsValid());
    EXPECT_EQ(m_Registry.RegisterSubmesh(fineKey, mesh, true, MeshGPUStorageDomain::ClusterFine), fine);
    EXPECT_EQ(m_Registry.Find(fine)->contentHash, fineEntry->contentHash);
}

TEST_F(MeshPoolGroupPlanTest, ClosingFineDrawsRetainsPhysicalStorageUntilExplicitRetirement)
{
    if (!HasCoherentDeviceLocalUploadMemory())
        GTEST_SKIP() << "fine domain requires reported coherent host-visible device-local upload memory";
    const auto mesh = MakeTriangle(false, false);
    const auto shared = m_Registry.RegisterSubmesh({m_Guid, 0}, mesh);
    const MeshGPUKey key{m_Guid, 1};
    const auto fine = m_Registry.RegisterSubmesh(key, mesh, false, MeshGPUStorageDomain::ClusterFine);
    const auto* entry = m_Registry.Find(fine);
    ASSERT_NE(entry, nullptr);
    MeshGPUEntryBindings physical;
    ASSERT_TRUE(m_Registry.TryGetDrawableBindings(*entry, physical));
    const auto row = entry->gpuMeshIndex;
    const auto capacity = m_Registry.GetStorageDomainCapacityBytes(MeshGPUStorageDomain::ClusterFine);
    EXPECT_TRUE(m_Registry.IsBufferInStorageDomain(physical.coreVB, MeshGPUStorageDomain::ClusterFine));
    EXPECT_TRUE(m_Registry.IsBufferInStorageDomain(physical.indexBuffer, MeshGPUStorageDomain::ClusterFine));
    EXPECT_FALSE(m_Registry.IsBufferInStorageDomain(physical.coreVB, MeshGPUStorageDomain::Shared));
    EXPECT_FALSE(m_Registry.IsBufferInStorageDomain({}, MeshGPUStorageDomain::ClusterFine));
    EXPECT_FALSE(m_Registry.CloseFineDraws(shared));
    const auto revision = m_Registry.GetContentRevision();
    ASSERT_TRUE(m_Registry.CloseFineDraws(fine));
    EXPECT_EQ(m_Registry.GetContentRevision(), revision + 1);
    EXPECT_TRUE(m_Registry.CloseFineDraws(fine));
    EXPECT_EQ(m_Registry.GetContentRevision(), revision + 1);
    entry = m_Registry.Find(fine);
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->gpuMeshIndex, row) << "logical closure must not recycle a live numeric row";
    EXPECT_EQ(m_Registry.GetStorageDomainCapacityBytes(MeshGPUStorageDomain::ClusterFine), capacity);
    EXPECT_FALSE(m_Registry.IsEntryDrawable(*entry));
    EXPECT_FALSE(m_Registry.IsFlushResident(*entry));
    MeshGPUEntryBindings refused = physical;
    EXPECT_FALSE(m_Registry.TryGetDrawableBindings(*entry, refused));
    EXPECT_EQ(refused, MeshGPUEntryBindings{});
    EXPECT_TRUE(m_Registry.IsBufferInStorageDomain(physical.coreVB, MeshGPUStorageDomain::ClusterFine));
    MeshPoolGroupPlan plan;
    plan.Refresh(m_Registry, MeshTableCount());
    EXPECT_EQ(plan.GroupOf(row), MeshPoolGroupPlan::kAbsentGroup);
    EXPECT_EQ(plan.ResidencyStats().SkippedNonResident, 0u) << "closure is intentional, not an incomplete upload";
    EXPECT_NE(plan.GroupOf(m_Registry.Find(shared)->gpuMeshIndex), MeshPoolGroupPlan::kAbsentGroup);
    EXPECT_FALSE(m_Registry.RegisterSubmesh(key, mesh, false, MeshGPUStorageDomain::ClusterFine).IsValid());
    EXPECT_FALSE(m_Registry.IsEntryDrawable(*m_Registry.Find(fine))) << "dedup cannot reopen a retired draw source";
    EXPECT_TRUE(m_Registry.UnregisterSubmesh(key));
    EXPECT_EQ(m_Registry.Find(fine), nullptr);
    EXPECT_FALSE(m_Registry.CloseFineDraws(fine));
}

TEST_F(MeshPoolGroupPlanTest, FineCapacityIncludesRetiringRangesAndRefusesUnboundedGrowth)
{
    if (!HasCoherentDeviceLocalUploadMemory())
        GTEST_SKIP() << "fine domain requires reported coherent host-visible device-local upload memory";
    Mesh mesh = MakeTriangle(false, false);
    // One large 16 MiB core allocation plus its index pool fits. Its sibling
    // growth would exceed the domain cap, even though logical payload is small.
    mesh.Vertices.resize((16ull << 20) / (8 * sizeof(float)));
    const MeshGPUKey firstKey{m_Guid, 0};
    const auto first = m_Registry.RegisterSubmesh(firstKey, mesh, false, MeshGPUStorageDomain::ClusterFine);
    ASSERT_NE(m_Registry.Find(first), nullptr);
    ASSERT_GT(m_Registry.Find(first)->indexCount, 0u);
    const auto owned = m_Registry.GetStorageDomainCapacityBytes(MeshGPUStorageDomain::ClusterFine);
    ASSERT_GT(owned, 0u);
    ASSERT_LE(owned, MeshGPURegistry::kMaximumClusterFineCapacityBytes);
    const auto refused = m_Registry.RegisterSubmesh({m_Guid, 1}, mesh, false, MeshGPUStorageDomain::ClusterFine);
    ASSERT_NE(m_Registry.Find(refused), nullptr);
    EXPECT_EQ(m_Registry.Find(refused)->indexCount, 0u);
    EXPECT_EQ(m_Registry.GetStorageDomainCapacityBytes(MeshGPUStorageDomain::ClusterFine), owned);
    ASSERT_TRUE(m_Registry.UnregisterSubmesh(firstKey));
    EXPECT_EQ(m_Registry.GetStorageDomainCapacityBytes(MeshGPUStorageDomain::ClusterFine), owned);
    const auto retiringRefusal = m_Registry.RegisterSubmesh({m_Guid, 2}, mesh, false, MeshGPUStorageDomain::ClusterFine);
    ASSERT_NE(m_Registry.Find(retiringRefusal), nullptr);
    EXPECT_EQ(m_Registry.Find(retiringRefusal)->indexCount, 0u);
    EXPECT_LE(m_Registry.GetStorageDomainCapacityBytes(MeshGPUStorageDomain::ClusterFine),
              MeshGPURegistry::kMaximumClusterFineCapacityBytes);
    // No graph or GPU reader exists in this allocator fixture. Once the normal
    // deferred-free boundary is passed, the same immutable failed key can heal.
    m_Registry.BeginFrame(100);
    const auto retried = m_Registry.RegisterSubmesh({m_Guid, 1}, mesh, false,
                                                   MeshGPUStorageDomain::ClusterFine);
    EXPECT_EQ(retried, refused);
    ASSERT_NE(m_Registry.Find(retried), nullptr);
    EXPECT_GT(m_Registry.Find(retried)->indexCount, 0u);
    EXPECT_EQ(m_Registry.GetStorageDomainCapacityBytes(MeshGPUStorageDomain::ClusterFine), owned);
}

TEST_F(MeshPoolGroupPlanTest, RefusedIndexPoolRollsBackOnlyTheUnpublishedFinePools)
{
    if (!HasCoherentDeviceLocalUploadMemory())
        GTEST_SKIP() << "fine domain requires reported coherent host-visible device-local upload memory";
    const auto ordinary = RegisterMesh(0, MakeTriangle(false, false));
    const auto sharedBytes = m_Registry.GetStorageDomainCapacityBytes(MeshGPUStorageDomain::Shared);
    Mesh oversized = MakeTriangle(false, false);
    oversized.Vertices.resize(MeshGPURegistry::kMaximumClusterFineCapacityBytes / (8 * sizeof(float)));
    const auto refused = m_Registry.RegisterSubmesh({m_Guid, 1}, oversized, false,
                                                    MeshGPUStorageDomain::ClusterFine);
    ASSERT_NE(m_Registry.Find(refused), nullptr);
    EXPECT_EQ(m_Registry.Find(refused)->indexCount, 0u);
    EXPECT_EQ(m_Registry.GetStorageDomainCapacityBytes(MeshGPUStorageDomain::ClusterFine), 0u)
        << "the new core pool must not strand the index pool's admission budget";
    const auto small = m_Registry.RegisterSubmesh({m_Guid, 2}, MakeTriangle(false, false), true,
                                                  MeshGPUStorageDomain::ClusterFine);
    ASSERT_NE(m_Registry.Find(small), nullptr);
    EXPECT_GT(m_Registry.Find(small)->indexCount, 0u);
    EXPECT_EQ(m_Registry.GetStorageDomainCapacityBytes(MeshGPUStorageDomain::Shared), sharedBytes);
    MeshPoolGroupPlan plan;
    plan.Refresh(m_Registry, MeshTableCount());
    EXPECT_NE(plan.GroupOf(ordinary), MeshPoolGroupPlan::kAbsentGroup)
        << "pre-existing shared geometry must survive the fine upload rollback";
}

TEST_F(MeshPoolGroupPlanTest, DifferentVertexFlagBucketsSplitGroups)
{
    const uint32_t meshA = RegisterMesh(0, MakeTriangle(false, false));
    const uint32_t meshB = RegisterMesh(1, MakeTriangle(true, false));
    ASSERT_NE(meshA, ~0u);
    ASSERT_NE(meshB, ~0u);

    MeshPoolGroupPlan plan;
    plan.Refresh(m_Registry, MeshTableCount());

    EXPECT_NE(plan.GroupOf(meshA), plan.GroupOf(meshB));
    EXPECT_EQ(plan.LiveGroupCount(), 2u);
}

TEST_F(MeshPoolGroupPlanTest, IndexTypeSplitsGroups)
{
    const uint32_t meshA = RegisterMesh(0, MakeTriangle(false, false));
    const uint32_t meshB = RegisterMesh(1, MakeTriangle(false, true));
    ASSERT_NE(meshA, ~0u);
    ASSERT_NE(meshB, ~0u);

    MeshPoolGroupPlan plan;
    plan.Refresh(m_Registry, MeshTableCount());

    // 16-bit vs 32-bit indices bind different index pools — never one draw.
    EXPECT_NE(plan.GroupOf(meshA), plan.GroupOf(meshB));
}

TEST_F(MeshPoolGroupPlanTest, IdsAreStableAndAppendOnlyAcrossRefresh)
{
    const uint32_t meshA = RegisterMesh(0, MakeTriangle(false, false));
    MeshPoolGroupPlan plan;
    plan.Refresh(m_Registry, MeshTableCount());
    const uint32_t groupA = plan.GroupOf(meshA);

    // A mid-frame registration of a new identity must only APPEND ids.
    const uint32_t meshB = RegisterMesh(1, MakeTriangle(true, false));
    plan.Refresh(m_Registry, MeshTableCount());

    EXPECT_EQ(plan.GroupOf(meshA), groupA);
    EXPECT_NE(plan.GroupOf(meshB), groupA);
    EXPECT_EQ(plan.GroupCount(), 2u);
}

// Renderer-core cannot include the engine-layer plan header, so the absent
// sentinel is duplicated on both sides of the draw stream; the scatter routes
// by one and consumers filter by the other, so they must stay equal.
TEST(MeshPoolGroupPlanContract, AbsentSentinelMatchesDrawStreamBuilder)
{
    EXPECT_EQ(MeshPoolGroupPlan::kAbsentGroup, GPUDrawStreamBuilder::kAbsentPoolGroup);
}

TEST_F(MeshPoolGroupPlanTest, UnmappedRowsReadAbsent)
{
    RegisterMesh(0, MakeTriangle(false, false));
    MeshPoolGroupPlan plan;
    // Larger table than the registry populates: the tail rows are absent.
    plan.Refresh(m_Registry, MeshTableCount() + 4u);
    EXPECT_EQ(plan.GroupOf(MeshTableCount() + 1u), MeshPoolGroupPlan::kAbsentGroup);
    // Out-of-bounds lookups are absent too (consumer guard path).
    EXPECT_EQ(plan.GroupOf(0xFFFFFFu), MeshPoolGroupPlan::kAbsentGroup);

    // The ordered axis mirrors the absence: unmapped rows read the sentinel.
    ASSERT_GT(plan.MeshToOrderedSpan().size(), MeshTableCount() + 1u);
    EXPECT_EQ(plan.MeshToOrderedSpan()[MeshTableCount() + 1u], MeshPoolGroupPlan::kAbsentGroup);
}

// Both-or-neither invariant at the source: a Refresh that finds ZERO live
// registry entries (scene-load streaming transient / teardown seam) must
// publish EMPTY spans on EVERY axis — a non-empty all-absent mesh map paired
// with an empty rank->group inverse would trip ScheduleUnifiedScatter's
// paired-span assert (live in DebugFast) and split the consolidation signal
// between consumers and the scatter.
TEST_F(MeshPoolGroupPlanTest, EmptyRegistrySnapshotPublishesEmptySpansOnEveryAxis)
{
    MeshPoolGroupPlan plan;
    plan.Refresh(m_Registry, /*meshTableCount=*/4u); // rows exist, none live
    EXPECT_TRUE(plan.MeshToGroupSpan().empty());
    EXPECT_TRUE(plan.MeshToOrderedSpan().empty());
    EXPECT_TRUE(plan.OrderedToGroupSpan().empty());
    EXPECT_EQ(plan.GroupOf(0u), MeshPoolGroupPlan::kAbsentGroup);
    EXPECT_EQ(plan.LiveGroupCount(), 0u);

    // And the plan recovers on the next Refresh once a mesh goes live.
    const uint32_t meshA = RegisterMesh(0, MakeTriangle(false, false));
    plan.Refresh(m_Registry, MeshTableCount());
    EXPECT_FALSE(plan.MeshToOrderedSpan().empty());
    EXPECT_EQ(plan.OrderedToGroupSpan().size(), 1u);
    EXPECT_NE(plan.GroupOf(meshA), MeshPoolGroupPlan::kAbsentGroup);
}

// The ordered mesh axis: dense unique ranks over the live meshes, sorted so a
// group's members are CONTIGUOUS (what lets the scatter scheduler span a
// group's per-mesh table rows with one published range and pack its region
// mesh-major), with OrderedToGroupSpan as the exact rank->group inverse.
TEST_F(MeshPoolGroupPlanTest, OrderedRanksAreDenseGroupContiguousAndInverseConsistent)
{
    // Interleave two identities so registration order differs from group order.
    const uint32_t meshA = RegisterMesh(0, MakeTriangle(false, false)); // g0
    const uint32_t meshB = RegisterMesh(1, MakeTriangle(true, false));  // g1
    const uint32_t meshC = RegisterMesh(2, MakeTriangle(false, false)); // g0
    const uint32_t meshD = RegisterMesh(3, MakeTriangle(true, false));  // g1

    MeshPoolGroupPlan plan;
    plan.Refresh(m_Registry, MeshTableCount());

    const auto ordered = plan.MeshToOrderedSpan();
    const auto inverse = plan.OrderedToGroupSpan();
    ASSERT_EQ(inverse.size(), 4u); // one rank per live mesh

    // Dense unique ranks covering [0, live).
    std::vector<bool> seen(4, false);
    for (uint32_t mesh : {meshA, meshB, meshC, meshD})
    {
        ASSERT_LT(mesh, ordered.size());
        const uint32_t rank = ordered[mesh];
        ASSERT_LT(rank, 4u);
        EXPECT_FALSE(seen[rank]) << "ranks must be unique per live mesh";
        seen[rank] = true;
        // Inverse agrees with the consumer-facing group map.
        EXPECT_EQ(inverse[rank], plan.GroupOf(mesh));
    }

    // Group members contiguous: same-group meshes hold adjacent ranks.
    EXPECT_EQ(plan.GroupOf(meshA), plan.GroupOf(meshC));
    EXPECT_EQ(ordered[meshC], ordered[meshA] + 1u);
    EXPECT_EQ(plan.GroupOf(meshB), plan.GroupOf(meshD));
    EXPECT_EQ(ordered[meshD], ordered[meshB] + 1u);

    // The inverse is group-sorted (runs never interleave).
    for (size_t i = 1; i < inverse.size(); ++i)
        EXPECT_LE(inverse[i - 1], inverse[i]);
}

} // namespace
