// HLOD runtime integration (design v0.2 §5, integration layer). End-to-end on a
// real GPUScene + device: bake a world's HLOD clusters to a .gehlod, reconcile it
// back (HlodRuntime), and drive the HLODSelectSystem's coverage switch. Verifies
// the integration glue the pure/logic tests (HlodBaker, HlodReconcile, HlodSelect,
// HlodEvictionStick) don't cover together: proxies spawn EVICTED while members
// stay resident, and a far/near camera flips residency between members and proxy.
//
// Device-gated (GTEST_SKIP without Vulkan): ReconcileScene registers proxy meshes
// through the real MeshGPURegistry, which needs a device.

#include <gtest/gtest.h>

#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/HlodRuntime.h"
#include "ECSModules/Rendering/Systems/HLODSelectSystem.h"
#include "ECSModules/Rendering/Systems/TransformHierarchySystem.h"
#include "Components/Hierarchy.h"

#include "Assets/HlodBakeDriver.h"
#include "Assets/HlodCache.h"
#include "Assets/ModelAsset.h"

#include "Components/Rendering/HLODProxy.h"
#include "Components/Rendering/HLODVolume.h"
#include "Components/Rendering/LocalBounds.h"
#include "Components/Rendering/MeshGPUData.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/RuntimeOnlyEntity.h"
#include "Components/SceneEntityTag.h"
#include "Components/Transform.h"

#include "ECS/Components.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"

#include "Rendering/Camera/CameraTypes.h"
#include "TestCameraEye.h"

#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Components;
using namespace GameEngine::Engine::Renderer;

#include "TestDeviceHelper.h"

namespace {

Mesh MakeBox(float extent) {
    Mesh mesh;
    mesh.PrimitiveTopology = MeshPrimitiveTopology::Triangles;
    Vertex a{}, b{}, c{};
    a.Position[1] = extent;
    b.Position[0] = -extent; b.Position[1] = -extent;
    c.Position[0] = extent;  c.Position[1] = -extent;
    a.Normal[2] = b.Normal[2] = c.Normal[2] = 1.0f;
    mesh.Vertices = {a, b, c};
    mesh.Indices = {0, 1, 2};
    mesh.MinBounds[0] = mesh.MinBounds[1] = mesh.MinBounds[2] = -extent;
    mesh.MaxBounds[0] = mesh.MaxBounds[1] = mesh.MaxBounds[2] = extent;
    return mesh;
}

struct ModelBank {
    std::unordered_map<GUID, SharedPtr<ModelAsset>> Models;
    const ModelAsset* Add(const GUID& guid, uint64 contentHash) {
        auto a = std::make_shared<ModelAsset>(guid, std::filesystem::path{});
        a->SetMeshesForTest({MakeBox(1.0f)});
        a->SetSourceContentHashForTest(contentHash);
        const ModelAsset* raw = a.get();
        Models[guid] = std::move(a);
        return raw;
    }
    Hlod::ModelResolver Resolver() {
        return [this](const GUID& g) -> SharedPtr<const ModelAsset> {
            auto it = Models.find(g);
            return it == Models.end() ? nullptr : it->second;
        };
    }
};

Components::WorldTransform WorldAt(float x, float y, float z) {
    Components::WorldTransform wt{};
    wt.Position = Mathematics::WorldPosition{x, y, z};
    return wt;
}

Components::SceneEntityTag Tag(const std::string& s) {
    Components::SceneEntityTag t{};
    std::memset(t.value, 0, sizeof(t.value));
    std::strncpy(t.value, s.c_str(), sizeof(t.value) - 1);
    return t;
}

Components::LocalBounds BoxBounds(float extent) {
    Components::LocalBounds lb{};
    lb.Box = Mathematics::BoundingBox::FromMinMax(
        Mathematics::Vector3{-extent, -extent, -extent},
        Mathematics::Vector3{extent, extent, extent});
    return lb;
}

// The near and culling range of the CameraAt() cameras.
constexpr Rendering::CameraRanges kCameraAtRanges{0.1f, 1000.0f};

// A perspective LOD probe at (x,y,z) with a vertical projection scale of one.
Rendering::CameraData CameraAt(float x, float y, float z) {
    Rendering::CameraData cam{};
    cam.view = Mathematics::Matrix4x4::Identity();
    cam.proj = Mathematics::Matrix4x4::Identity();
    cam.proj[3][3] = 0.0f;
    cam.viewProj = cam.proj;
    cam.cameraPos.x = x;
    cam.cameraPos.y = y;
    cam.cameraPos.z = z;
    cam.cameraPos.w = 0.0f; // not a 2D ortho view -> LOD enabled
    return cam;
}

std::filesystem::path TempPath(const char* name) {
    return std::filesystem::temp_directory_path() / name;
}

// One cell of `count` FAR statics, each a distinct model (r = 1 so the benefit
// gate admits), tagged for stable reconcile. Placed around x0 so the cluster sits
// far from the origin and a coverage test can drive a proxy switch.
void SpawnCluster(ECS::World& world, ModelBank& bank, int count, float x0, const GUID& matGuid) {
    for (int i = 0; i < count; ++i) {
        const GUID modelGuid = GUID::Derive(GUID::Null(), "reconcile/model/" + std::to_string(i));
        const ModelAsset* model = bank.Add(modelGuid, 0x9000u + static_cast<uint64>(i));
        (void)model;
        ECS::Entity e = world.Create();
        e.Set(WorldAt(x0 + static_cast<float>(i) * 2.0f, 0.0f, 0.0f));
        e.Set(BoxBounds(1.0f));
        e.Set(Tag("member_" + std::to_string(i)));
        MeshRenderer mr{};
        mr.materialAssetGuid.Set(matGuid);
        mr.modelAssetGuid.Set(modelGuid);
        e.Set(mr);
        // The GPUScene bridge extraction would add; the test drives the select
        // switch without extraction, so seed it so the residency flag has a home.
        e.Set(MeshGPUData{});
    }
}

void AddVolume(ECS::World& world) {
    ECS::Entity v = world.Create();
    HLODVolume vol{};
    vol.CellSize = 1000.0f;    // whole cluster in one cell
    vol.MaxInstancingRatio = 4.0f;
    vol.MinMembers = 8u;
    vol.VBBudgetMB = 256u;
    vol.SwitchCoverage = 0.08f;
    vol.SwitchHysteresis = 0.25f;
    v.Set(vol);
}

constexpr int kMemberCount = 10;

// Bake + reconcile the world as it is; returns the .gehlod path (caller removes it).
std::filesystem::path BakeAndReconcileWorld(RenderServices& rs, ECS::World& world, ModelBank& bank,
                                            Hlod::HlodRuntime& runtime, const char* fileName) {
    const std::filesystem::path out = TempPath(fileName);
    std::filesystem::remove(out);
    const Hlod::BakeDriverResult bake = Hlod::BakeHlodForWorld(world, bank.Resolver(), out);
    EXPECT_EQ(bake.Outcome, Hlod::BakeOutcome::Wrote);

    runtime.ReconcileScene(world, rs.GetMeshGPURegistry(), out, "test", bank.Resolver());
    return out;
}

// Bake + reconcile a fresh world; returns the .gehlod path (caller removes it).
std::filesystem::path BakeAndReconcile(RenderServices& rs, ECS::World& world, ModelBank& bank,
                                       Hlod::HlodRuntime& runtime, const GUID& matGuid,
                                       float clusterX, const char* fileName) {
    AddVolume(world);
    SpawnCluster(world, bank, kMemberCount, clusterX, matGuid);
    world.ProcessCommands();
    return BakeAndReconcileWorld(rs, world, bank, runtime, fileName);
}

ECS::EntityHandle FindTagged(ECS::World& world, const std::string& name) {
    ECS::EntityHandle found{};
    world.Query<ECS::Read<SceneEntityTag>>().Each([&](ECS::EntityHandle e, const SceneEntityTag& tag) {
        if (tag.View() == name)
            found = e;
    });
    return found;
}

// A still scene: the hierarchy and the select system run each frame and nothing moves.
// The destroyed member is alone in its archetype, so its chunk empties and gets no column stamp.
void ExpectDestroyedMemberMakesClusterStale(Rendering::IDevice* device, bool parented, const char* fileName) {
    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device));
    auto world = std::make_unique<ECS::World>(nullptr);
    ModelBank bank;
    Hlod::HlodRuntime& runtime = rs.GetHlodRuntime();
    TransformHierarchySystem hierarchy;
    HLODSelectSystem select(&rs);
    constexpr float kDt = 1.0f / 60.0f;

    AddVolume(*world);
    SpawnCluster(*world, bank, kMemberCount, 0.0f, GUID::Derive(GUID::Null(), fileName));
    world->ProcessCommands();
    const ECS::EntityHandle lone = FindTagged(*world, "member_0");
    ASSERT_TRUE(lone.IsValid());
    world->AddComponent(lone, Transform::FromTRS(Mathematics::WorldPosition{0.0, 0.0, 0.0}, Mathematics::Quaternion{},
                                                 Mathematics::Vector3{1.0f, 1.0f, 1.0f}));
    if (parented) {
        // An identity root, so the member is a hierarchy node and its world pose does not change.
        ECS::Entity anchor = world->Create();
        anchor.Set(Transform{});
        anchor.Set(WorldTransform{});
        world->AddComponent(lone, Parent{anchor.GetHandle()});
    }
    world->ProcessCommands();
    // The first update writes every pose. Do it before the reconcile records the member versions.
    hierarchy.Update(*world, kDt);
    const std::filesystem::path out = BakeAndReconcileWorld(rs, *world, bank, runtime, fileName);
    ASSERT_EQ(runtime.GetClusters().size(), 1u);

    Rendering::CameraId cam = rs.Views().AllocateCamera("HlodDestroyedMemberCam");
    rs.Views().SetViewpointCamera(cam);
    rs.Views().SetCameraData(cam, ::GameEngine::Tests::TestCameraWithEye(CameraAt(0.0f, 0.0f, -2000.0f)), kCameraAtRanges);
    for (int frame = 0; frame < 3; ++frame) {
        hierarchy.Update(*world, kDt);
        select.Update(*world, kDt);
    }
    ASSERT_TRUE(runtime.GetClusters()[0].ProxyActive);
    ASSERT_FALSE(runtime.GetClusters()[0].Stale);

    world->DestroyEntity(lone);
    world->ProcessCommands();
    for (int frame = 0; frame < 2; ++frame) {
        hierarchy.Update(*world, kDt);
        select.Update(*world, kDt);
    }
    EXPECT_TRUE(runtime.GetClusters()[0].Stale) << "a destroyed member must mark the cluster stale";
    EXPECT_FALSE(runtime.GetClusters()[0].ProxyActive) << "the proxy must not stand in for a destroyed member";

    runtime.Retire(*world, rs.GetMeshGPURegistry());
    world.reset();
    rs.Shutdown();
    std::error_code ec;
    std::filesystem::remove(out, ec);
}

} // namespace

TEST(HlodRuntimeReconcile, FreshSceneResolvesParentedMembersBeforeValidation) {
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));
    ModelBank bank;
    const GUID material = GUID::Derive(GUID::Null(), "reconcile/fresh-scene");
    const auto populate = [&bank, material](ECS::World& world)
    {
        AddVolume(world);
        SpawnCluster(world, bank, kMemberCount, 0.0f, material);
        auto parent = world.Create();
        parent.Set(Transform::FromTRS(Mathematics::WorldPosition{40.0, 0.0, 65.0},
                                     Mathematics::Quaternion{}, Mathematics::Vector3{1.0f, 1.0f, 1.0f}));
        world.ProcessCommands();
        for (int i = 0; i < kMemberCount; ++i)
        {
            const auto member = FindTagged(world, "member_" + std::to_string(i));
            ASSERT_TRUE(member.IsValid());
            world.AddComponent(member, Parent{parent.GetHandle()});
            world.AddComponent(member, Transform::FromTRS(Mathematics::WorldPosition{i * 2.0, 0.0, 0.0},
                                                          Mathematics::Quaternion{},
                                                          Mathematics::Vector3{1.0f, 1.0f, 1.0f}));
        }
        world.ProcessCommands();
    };
    const auto out = TempPath("hlod_reconcile_fresh_scene.gehlod");
    std::filesystem::remove(out);
    {
        auto authored = std::make_unique<ECS::World>(nullptr);
        populate(*authored);
        TransformHierarchySystem hierarchy;
        hierarchy.Update(*authored, 0.0f);
        ASSERT_EQ(Hlod::BakeHlodForWorld(*authored, bank.Resolver(), out).Outcome, Hlod::BakeOutcome::Wrote);
    }

    auto loaded = std::make_unique<ECS::World>(nullptr);
    populate(*loaded);
    const auto member = FindTagged(*loaded, "member_0");
    ASSERT_EQ(loaded->GetComponent<WorldTransform>(member)->Version, 0u);
    auto& runtime = rs.GetHlodRuntime();
    const uint32 clusters = runtime.ReconcileScene(*loaded, rs.GetMeshGPURegistry(), out, "test", bank.Resolver());
    std::filesystem::remove(out);
    ASSERT_EQ(clusters, 1u);
    const auto& cluster = runtime.GetClusters().front();
    EXPECT_FALSE(cluster.Stale);
    EXPECT_FALSE(cluster.Proxies.empty());
    EXPECT_EQ(loaded->GetComponent<WorldTransform>(member)->Position.Z, 65.0);
    runtime.Retire(*loaded, rs.GetMeshGPURegistry());
}

TEST(HlodRuntimeReconcile, ReconcileSpawnsEvictedProxiesAndTagsMembers) {
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));
    auto world = std::make_unique<ECS::World>(nullptr);
    ModelBank bank;
    Hlod::HlodRuntime& runtime = rs.GetHlodRuntime();

    const GUID matGuid = GUID::Derive(GUID::Null(), "reconcile/material");
    const std::filesystem::path out =
        BakeAndReconcile(rs, *world, bank, runtime, matGuid, 0.0f, "hlod_reconcile_spawn.gehlod");

    ASSERT_EQ(runtime.GetClusters().size(), 1u);
    const Hlod::RuntimeCluster& cluster = runtime.GetClusters()[0];
    EXPECT_EQ(cluster.Members.size(), static_cast<size_t>(kMemberCount));
    EXPECT_FALSE(cluster.Stale);
    ASSERT_FALSE(cluster.Proxies.empty()) << "an admitted cluster must spawn a proxy";
    EXPECT_FALSE(cluster.ProxyActive) << "proxies start evicted; members are the resident set";

    // Every proxy spawned EVICTED (members are what renders until a far switch).
    for (ECS::EntityHandle proxy : cluster.Proxies) {
        EXPECT_TRUE(world->HasComponent<HLODProxy>(proxy));
        EXPECT_TRUE(world->HasComponent<RuntimeOnlyEntity>(proxy));
        const auto* gpu = world->GetComponent<MeshGPUData>(proxy);
        ASSERT_NE(gpu, nullptr);
        EXPECT_TRUE(gpu->hlodEvicted) << "proxy must spawn evicted";
    }

    // Every member stays resident (NOT evicted) after reconcile.
    for (ECS::EntityHandle member : cluster.Members) {
        const auto* gpu = world->GetComponent<MeshGPUData>(member);
        if (gpu)
        {
            EXPECT_FALSE(gpu->hlodEvicted);
        }
        // C6: eviction never disables the entity.
        ASSERT_NE(world->GetComponent<MeshRenderer>(member), nullptr);
        EXPECT_TRUE(ECS::Entity(world.get(), member).IsEnabled<MeshRenderer>());
    }

    // C7: retire must release every proxy mesh registration — it is the ONLY
    // release path. SceneDocumentManager mirrors this call before World::Clear
    // on File > New (Clear restarts entity versions, so surviving cluster
    // handles would alias freshly-seeded entities).
    const std::vector<GameEngine::Rendering::MeshGPUKey> proxyKeys = cluster.ProxyMeshKeys;
    ASSERT_FALSE(proxyKeys.empty());
    runtime.Retire(*world, rs.GetMeshGPURegistry());
    EXPECT_TRUE(runtime.GetClusters().empty());
    for (const GameEngine::Rendering::MeshGPUKey& key : proxyKeys)
        EXPECT_EQ(rs.GetMeshGPURegistry().FindByKey(key), nullptr)
            << "proxy mesh must be unregistered on retire";

    world.reset();
    rs.Shutdown();
    std::error_code ec;
    std::filesystem::remove(out, ec);
}

TEST(HlodRuntimeReconcile, LateRetirePreservesIncomingEntityWithRecycledProxyHandle)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    RenderServices services;
    ASSERT_TRUE(services.Initialize(device.get()));
    auto world = std::make_unique<ECS::World>(nullptr);
    ModelBank bank;
    auto& runtime = services.GetHlodRuntime();
    const auto material = GUID::Derive(GUID::Null(), "reconcile/late-retire");
    const auto cache = BakeAndReconcile(
        services, *world, bank, runtime, material, 0.0f, "hlod_reconcile_late_retire.gehlod");

    ASSERT_TRUE(runtime.IsBoundTo(*world));
    ASSERT_EQ(runtime.GetClusters().size(), 1u);
    const auto& cluster = runtime.GetClusters().front();
    ASSERT_FALSE(cluster.Proxies.empty());
    ASSERT_FALSE(cluster.ProxyMeshKeys.empty());
    const auto outgoingProxy = cluster.Proxies.front();
    const auto proxyMeshes = cluster.ProxyMeshKeys;
    const auto outgoingCount = world->GetEntityCount();

    world->Clear();
    world->ProcessCommands();
    EXPECT_FALSE(runtime.IsBoundTo(*world));

    ECS::EntityHandle incoming{};
    for (size_t index = 0; index < outgoingCount; ++index)
    {
        incoming = world->CreateEntity();
        if (incoming == outgoingProxy)
            break;
    }
    ASSERT_EQ(incoming, outgoingProxy);
    ASSERT_TRUE(world->IsValid(incoming));

    runtime.Retire(*world, services.GetMeshGPURegistry());
    world->ProcessCommands();
    EXPECT_TRUE(world->IsValid(incoming));
    EXPECT_TRUE(runtime.GetClusters().empty());
    for (const auto& key : proxyMeshes)
        EXPECT_EQ(services.GetMeshGPURegistry().FindByKey(key), nullptr);

    world.reset();
    services.Shutdown();
    std::error_code error;
    std::filesystem::remove(cache, error);
}

// Members in one cell on two layers bake into one cluster per layer set, and each proxy draws on its
// members' layers: no proxy draws a member in a view that does not draw that member.
TEST(HlodRuntimeReconcile, ProxyLayersEqualTheirMembersLayers) {
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));
    auto world = std::make_unique<ECS::World>(nullptr);
    ModelBank bank;
    Hlod::HlodRuntime& runtime = rs.GetHlodRuntime();

    const GUID matGuid = GUID::Derive(GUID::Null(), "reconcile/material-layers");
    AddVolume(*world);
    for (int i = 0; i < 2 * kMemberCount; ++i) {
        const GUID modelGuid = GUID::Derive(GUID::Null(), "reconcile/layers/model/" + std::to_string(i));
        bank.Add(modelGuid, 0xA000u + static_cast<uint64>(i));
        ECS::Entity e = world->Create();
        e.Set(WorldAt(static_cast<float>(i) * 2.0f, 0.0f, 0.0f));
        e.Set(BoxBounds(1.0f));
        e.Set(Tag("layered_" + std::to_string(i)));
        MeshRenderer mr{};
        mr.materialAssetGuid.Set(matGuid);
        mr.modelAssetGuid.Set(modelGuid);
        mr.RenderLayers = (i % 2 == 0) ? Rendering::RenderLayerMask::Default() : Rendering::RenderLayerMask::FromLayer(3);
        e.Set(mr);
        e.Set(MeshGPUData{});
    }
    world->ProcessCommands();

    const std::filesystem::path out = TempPath("hlod_reconcile_layers.gehlod");
    std::filesystem::remove(out);
    ASSERT_EQ(Hlod::BakeHlodForWorld(*world, bank.Resolver(), out).Outcome, Hlod::BakeOutcome::Wrote);
    runtime.ReconcileScene(*world, rs.GetMeshGPURegistry(), out, "test", bank.Resolver());

    ASSERT_EQ(runtime.GetClusters().size(), 2u) << "one cluster per layer set in the cell";
    for (const Hlod::RuntimeCluster& cluster : runtime.GetClusters()) {
        ASSERT_FALSE(cluster.Proxies.empty());
        ASSERT_FALSE(cluster.Members.empty());
        for (ECS::EntityHandle proxy : cluster.Proxies) {
            const auto* proxyRenderer = world->GetComponent<MeshRenderer>(proxy);
            ASSERT_NE(proxyRenderer, nullptr);
            for (ECS::EntityHandle member : cluster.Members)
                EXPECT_EQ(proxyRenderer->RenderLayers, world->GetComponent<MeshRenderer>(member)->RenderLayers);
        }
    }

    runtime.Retire(*world, rs.GetMeshGPURegistry());
    world.reset();
    rs.Shutdown();
    std::error_code ec;
    std::filesystem::remove(out, ec);
}

TEST(HlodRuntimeReconcile, SelectSystemFlipsResidencyByCoverage) {
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));
    auto world = std::make_unique<ECS::World>(nullptr);
    ModelBank bank;
    Hlod::HlodRuntime& runtime = rs.GetHlodRuntime();

    const GUID matGuid = GUID::Derive(GUID::Null(), "reconcile/material2");
    const std::filesystem::path out =
        BakeAndReconcile(rs, *world, bank, runtime, matGuid, 0.0f, "hlod_reconcile_flip.gehlod");
    ASSERT_EQ(runtime.GetClusters().size(), 1u);

    // A viewpoint camera the select system reads. Start it far so the cluster
    // (center ~ (9,0,0), radius ~10) subtends little screen -> proxy-active.
    Rendering::CameraId cam = rs.Views().AllocateCamera("HlodReconcileCam");
    rs.Views().SetViewpointCamera(cam);

    HLODSelectSystem select(&rs);
    constexpr float kDt = 1.0f / 60.0f;

    // FAR: coverage = radius * 1 / dist ~ 10 / 2000 = 0.005 < enter (0.08) -> proxy.
    rs.Views().SetCameraData(cam, ::GameEngine::Tests::TestCameraWithEye(CameraAt(0.0f, 0.0f, -2000.0f)), kCameraAtRanges);
    select.Update(*world, kDt);

    ASSERT_TRUE(rs.ConsumeResidencyExtractionPending())
        << "a residency flip must request the full extraction lane";
    EXPECT_TRUE(runtime.GetClusters()[0].ProxyActive);
    for (ECS::EntityHandle member : runtime.GetClusters()[0].Members) {
        const auto* gpu = world->GetComponent<MeshGPUData>(member);
        ASSERT_NE(gpu, nullptr);
        EXPECT_TRUE(gpu->hlodEvicted) << "far cluster: members evicted";
    }
    for (ECS::EntityHandle proxy : runtime.GetClusters()[0].Proxies) {
        const auto* gpu = world->GetComponent<MeshGPUData>(proxy);
        ASSERT_NE(gpu, nullptr);
        EXPECT_FALSE(gpu->hlodEvicted) << "far cluster: proxy resident";
    }

    // NEAR: camera close to the cluster -> coverage high -> members back.
    rs.Views().SetCameraData(cam, ::GameEngine::Tests::TestCameraWithEye(CameraAt(9.0f, 0.0f, -12.0f)), kCameraAtRanges);
    select.Update(*world, kDt);

    EXPECT_TRUE(rs.ConsumeResidencyExtractionPending());
    EXPECT_FALSE(runtime.GetClusters()[0].ProxyActive);
    for (ECS::EntityHandle member : runtime.GetClusters()[0].Members) {
        const auto* gpu = world->GetComponent<MeshGPUData>(member);
        ASSERT_NE(gpu, nullptr);
        EXPECT_FALSE(gpu->hlodEvicted) << "near cluster: members resident";
    }
    for (ECS::EntityHandle proxy : runtime.GetClusters()[0].Proxies) {
        const auto* gpu = world->GetComponent<MeshGPUData>(proxy);
        ASSERT_NE(gpu, nullptr);
        EXPECT_TRUE(gpu->hlodEvicted) << "near cluster: proxy evicted";
    }

    rs.Views().SetCameraData(cam, ::GameEngine::Tests::TestCameraWithEye(CameraAt(0.0f, 0.0f, -2000.0f)),
                             kCameraAtRanges);
    select.Update(*world, kDt);
    ASSERT_TRUE(runtime.GetClusters()[0].ProxyActive);
    const auto members = runtime.GetClusters()[0].Members;
    runtime.Retire(*world, rs.GetMeshGPURegistry());
    EXPECT_FALSE(runtime.HasClusters());
    for (ECS::EntityHandle member : members)
        EXPECT_FALSE(world->GetComponent<MeshGPUData>(member)->hlodEvicted);
    world.reset();
    rs.Shutdown();
    std::error_code ec;
    std::filesystem::remove(out, ec);
}

TEST(HlodRuntimeReconcile, LosingADrawableProxyRestoresMembersAtTheSamePose)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    RenderServices services;
    ASSERT_TRUE(services.Initialize(device.get()));
    auto world = std::make_unique<ECS::World>(nullptr);
    ModelBank models;
    auto& runtime = services.GetHlodRuntime();
    const GUID material = GUID::Derive(GUID::Null(), "reconcile/lost-proxy");
    BakeAndReconcile(services, *world, models, runtime, material, 0.0f, "hlod_lost_proxy.gehlod");
    ASSERT_EQ(runtime.GetClusters().size(), 1u);
    auto& cluster = runtime.GetClusters().front();
    ASSERT_FALSE(cluster.ProxyMeshKeys.empty());

    const auto camera = services.Views().AllocateCamera("HLOD fallback");
    services.Views().SetViewpointCamera(camera);
    services.Views().SetCameraData(camera, ::GameEngine::Tests::TestCameraWithEye(CameraAt(0.0f, 0.0f, -2000.0f)),
                                   kCameraAtRanges);
    HLODSelectSystem select(&services);
    select.Update(*world, 0.0f);
    ASSERT_TRUE(cluster.ProxyActive);
    services.GetMeshGPURegistry().UnregisterSubmesh(cluster.ProxyMeshKeys.front());
    select.Update(*world, 0.0f);

    EXPECT_FALSE(cluster.ProxyActive);
    for (const auto member : cluster.Members)
    {
        const auto* gpu = world->GetComponent<MeshGPUData>(member);
        ASSERT_NE(gpu, nullptr);
        EXPECT_FALSE(gpu->hlodEvicted);
    }
    runtime.Retire(*world, services.GetMeshGPURegistry());
    world.reset();
    services.Shutdown();
}

TEST(HlodRuntimeReconcile, ProxyWaitsForTheBakedMemberLodAndWholeGeometry)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    RenderServices services;
    ASSERT_TRUE(services.Initialize(device.get()));
    auto world = std::make_unique<ECS::World>(nullptr);
    ModelBank bank;
    const GUID material = GUID::Derive(GUID::Null(), "reconcile/member-lod");
    AddVolume(*world);
    SpawnCluster(*world, bank, kMemberCount, 0.0f, material);
    world->ProcessCommands();
    for (auto& [guid, model] : bank.Models)
    {
        Mesh mesh = MakeBox(1.0f);
        mesh.AuthoredLODs = true;
        mesh.ExtraLODs = {mesh.Indices, mesh.Indices};
        mesh.ExtraLODCoverage = {0.25f, 0.004f};
        model->SetMeshesForTest({std::move(mesh)});
        ASSERT_EQ(services.GetMeshGPURegistry().RegisterModelMeshes(guid, *model).size(), 1u);
    }
    auto& runtime = services.GetHlodRuntime();
    const auto sidecar =
        BakeAndReconcileWorld(services, *world, bank, runtime, "hlod_member_lod_envelope.gehlod");
    ASSERT_EQ(runtime.GetClusters().size(), 1u);
    auto& cluster = runtime.GetClusters().front();
    EXPECT_FALSE(cluster.Stale);

    auto& views = services.Views();
    const auto camera = views.AllocateCamera("HLOD member LOD");
    views.SetViewpointCamera(camera);
    HLODSelectSystem select(&services);
    const auto pose = [&](float distance)
    {
        views.SetCameraData(camera, Tests::TestCameraWithEye(CameraAt(9.0f, 0.0f, -distance)),
                            kCameraAtRanges);
        select.Update(*world, 0.0f);
    };
    pose(200.0f);
    EXPECT_FALSE(cluster.ProxyActive);

    auto* memberGpu = world->GetComponentForWrite<MeshGPUData>(cluster.Members.front());
    ASSERT_NE(memberGpu, nullptr);
    memberGpu->clusterSplitOwner = true;
    pose(2000.0f);
    EXPECT_FALSE(cluster.ProxyActive);
    memberGpu->clusterSplitOwner = false;
    pose(2000.0f);
    EXPECT_TRUE(cluster.ProxyActive);

    services.SetLODForceLevel(1);
    pose(2000.0f);
    EXPECT_FALSE(cluster.ProxyActive);
    services.SetLODForceLevel(2);
    pose(2000.0f);
    EXPECT_TRUE(cluster.ProxyActive);
    services.SetLODForceLevel(0xFFFFFFFFu);
    pose(200.0f);
    EXPECT_FALSE(cluster.ProxyActive);

    runtime.Retire(*world, services.GetMeshGPURegistry());
    std::filesystem::remove(sidecar);
    services.Shutdown();
    device->Shutdown();
}

TEST(HlodRuntimeReconcile, ChangedCookedGeometryRestoresSourceMeshes)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));
    auto world = std::make_unique<ECS::World>(nullptr);
    ModelBank bank;
    AssetEventDispatcher events;
    HLODSelectSystem select(&rs, &events);
    auto& runtime = rs.GetHlodRuntime();
    const GUID material = GUID::Derive(GUID::Null(), "reconcile/live-recook-material");
    const auto file = BakeAndReconcile(rs, *world, bank, runtime, material, 0.0f, "hlod_reconcile_recook.gehlod");
    ASSERT_EQ(runtime.GetClusters().size(), 1u);
    const auto& cluster = runtime.GetClusters().front();
    ASSERT_FALSE(cluster.Stale);
    ASSERT_FALSE(cluster.MemberMeshSources.empty());

    const auto cam = rs.Views().AllocateCamera("HlodRecookCamera");
    rs.Views().SetViewpointCamera(cam);
    rs.Views().SetCameraData(cam, ::GameEngine::Tests::TestCameraWithEye(CameraAt(0.0f, 0.0f, -2000.0f)),
                             kCameraAtRanges);
    select.Update(*world, 0.0f);
    ASSERT_TRUE(cluster.ProxyActive);
    EXPECT_TRUE(rs.ConsumeResidencyExtractionPending());

    const GUID edited = cluster.MemberMeshSources.front().Model;
    events.DispatchEvent(AssetEvent{AssetEventType::AssetReloaded, GUID::Generate(), AssetType::Model, ""});
    events.DispatchEvent(AssetEvent{AssetEventType::AssetReloaded, edited, AssetType::Model, ""});
    select.Update(*world, 0.0f);
    EXPECT_TRUE(cluster.ProxyActive);
    EXPECT_FALSE(cluster.Stale);
    EXPECT_FALSE(rs.ConsumeResidencyExtractionPending());

    ModelAsset& model = *bank.Models.at(edited);
    auto mesh = model.GetMesh(0);
    mesh.Vertices[0].Position[0] += 1.0f;
    model.SetMeshesForTest({std::move(mesh)});
    events.DispatchEvent(AssetEvent{AssetEventType::AssetReloaded, edited, AssetType::Model, ""});
    select.Update(*world, 0.0f);
    EXPECT_TRUE(cluster.Stale);
    EXPECT_FALSE(cluster.ProxyActive);
    EXPECT_TRUE(rs.ConsumeResidencyExtractionPending());
    for (auto entity : cluster.Members)
        EXPECT_FALSE(world->GetComponent<MeshGPUData>(entity)->hlodEvicted);
    runtime.Retire(*world, rs.GetMeshGPURegistry());
    std::filesystem::remove(file);
    rs.Shutdown();
    device->Shutdown();
}

TEST(HlodRuntimeReconcile, ASecondViewCanKeepTheMembersResident)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    RenderServices services;
    ASSERT_TRUE(services.Initialize(device.get()));
    auto world = std::make_unique<ECS::World>(nullptr);
    ModelBank models;
    auto& runtime = services.GetHlodRuntime();
    const GUID material = GUID::Derive(GUID::Null(), "reconcile/near-second-view");
    BakeAndReconcile(services, *world, models, runtime, material, 0.0f, "hlod_second_view.gehlod");
    ASSERT_EQ(runtime.GetClusters().size(), 1u);
    const auto& cluster = runtime.GetClusters().front();

    auto& views = services.Views();
    const auto overview = views.AllocateCamera("HLOD overview");
    views.SetViewpointCamera(overview);
    views.SetCameraData(overview, ::GameEngine::Tests::TestCameraWithEye(CameraAt(0.0f, 0.0f, -2000.0f)),
                        kCameraAtRanges);
    const auto mainView = views.AllocateView("HLOD overview", overview);
    views.SetViewWorldId(mainView, world->GetWorldId());
    const auto detail = views.AllocateCamera("HLOD detail");
    views.SetCameraData(detail, ::GameEngine::Tests::TestCameraWithEye(CameraAt(9.0f, 0.0f, -12.0f)),
                        kCameraAtRanges);
    const auto secondView = views.AllocateView("HLOD detail", detail);
    views.SetViewWorldId(secondView, world->GetWorldId() + 1u);

    HLODSelectSystem select(&services);
    select.Update(*world, 0.0f);
    EXPECT_TRUE(cluster.ProxyActive) << "a view of another world has no demand";

    views.SetViewWorldId(secondView, world->GetWorldId());
    views.SetViewRenderLayerMask(secondView, Rendering::RenderLayerMask::None());
    select.Update(*world, 0.0f);
    EXPECT_TRUE(cluster.ProxyActive) << "a view excluding the cluster's layers has no demand";

    views.SetViewRenderLayerMask(secondView, cluster.RenderLayers);
    select.Update(*world, 0.0f);
    EXPECT_FALSE(cluster.ProxyActive) << "the near view must see the members";
    ASSERT_TRUE(views.ReleaseView(secondView));
    select.Update(*world, 0.0f);
    EXPECT_TRUE(cluster.ProxyActive) << "the overview alone can use the proxy again";

    runtime.Retire(*world, services.GetMeshGPURegistry());
    world.reset();
    services.Shutdown();
}

// S1 (stale HLOD proxies): the production reconcile recomputes the expected
// ConfigHash from the live HLODVolume and the CURRENT cook-semantics versions.
// A .gehlod keyed under different semantics (e.g. baked before a
// kLodGeneratorVersion bump) must be rejected fail-visibly — members-only, no
// proxies — never silently drawn beside freshly-cooked members.
TEST(HlodRuntimeReconcile, StaleConfigHashBakeIsRejectedAtReconcile) {
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));
    auto world = std::make_unique<ECS::World>(nullptr);
    ModelBank bank;
    Hlod::HlodRuntime& runtime = rs.GetHlodRuntime();

    const GUID matGuid = GUID::Derive(GUID::Null(), "reconcile/material4");
    const std::filesystem::path out = BakeAndReconcile(
        rs, *world, bank, runtime, matGuid, 0.0f, "hlod_reconcile_staleconfig.gehlod");
    ASSERT_EQ(runtime.GetClusters().size(), 1u) << "fresh bake must reconcile";

    // Re-key the bake as if it were produced under different cook semantics.
    Hlod::HlodBakedScene baked;
    ASSERT_EQ(Hlod::ReadHlodCache(out, nullptr, baked), Hlod::HlodCacheStatus::Hit);
    baked.Key.ConfigHash ^= 0x1ull;
    ASSERT_TRUE(Hlod::WriteHlodCache(out, baked));

    const uint32 clusters =
        runtime.ReconcileScene(*world, rs.GetMeshGPURegistry(), out, "test", bank.Resolver());
    EXPECT_EQ(clusters, 0u) << "a config/version-stale bake reconciled clusters";
    EXPECT_TRUE(runtime.GetClusters().empty());

    world.reset();
    rs.Shutdown();
    std::error_code ec;
    std::filesystem::remove(out, ec);
}

TEST(HlodRuntimeReconcile, OnlyEditsToMemberSourcesInvalidateAnActiveProxy)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    RenderServices services;
    ASSERT_TRUE(services.Initialize(device.get()));
    auto world = std::make_unique<ECS::World>(nullptr);
    ModelBank bank;
    auto& runtime = services.GetHlodRuntime();
    const GUID material = GUID::Derive(GUID::Null(), "reconcile/changed-member-material");
    const auto file = BakeAndReconcile(services, *world, bank, runtime, material, 0.0f,
                                       "hlod_reconcile_changed_member_material.gehlod");
    ASSERT_EQ(runtime.GetClusters().size(), 1u);
    auto& cluster = runtime.GetClusters().front();
    const ECS::EntityHandle member = cluster.Members.front();

    // Workspace edits may affect the same component types without affecting an HLOD member.
    const auto unrelated = world->Create().Set(WorldAt(500.0f, 0.0f, 0.0f)).Set(MeshRenderer{}).GetHandle();
    world->ProcessCommands();
    const auto camera = services.Views().AllocateCamera("HlodMemberMaterialCam");
    services.Views().SetViewpointCamera(camera);
    services.Views().SetCameraData(camera,
        ::GameEngine::Tests::TestCameraWithEye(CameraAt(0.0f, 0.0f, -2000.0f)), kCameraAtRanges);
    HLODSelectSystem select(&services);
    constexpr float kDt = 1.0f / 60.0f;
    select.Update(*world, kDt);
    ASSERT_TRUE(cluster.ProxyActive);

    const auto structure = world->GetStructuralChangeVersion();
    const GUID editedMaterial = GUID::Derive(GUID::Null(), "reconcile/edited-material");
    auto* unrelatedRenderer = world->GetComponentForWrite<MeshRenderer>(unrelated);
    ASSERT_NE(unrelatedRenderer, nullptr);
    unrelatedRenderer->materialAssetGuid.Set(editedMaterial);
    select.Update(*world, kDt);
    EXPECT_TRUE(cluster.ProxyActive);
    EXPECT_FALSE(cluster.Stale);

    auto* memberRenderer = world->GetComponentForWrite<MeshRenderer>(member);
    ASSERT_NE(memberRenderer, nullptr);
    memberRenderer->materialAssetGuid.Set(editedMaterial);
    EXPECT_EQ(world->GetStructuralChangeVersion(), structure);
    select.Update(*world, kDt);
    EXPECT_TRUE(cluster.Stale);
    EXPECT_FALSE(cluster.ProxyActive);

    runtime.Retire(*world, services.GetMeshGPURegistry());
    world.reset();
    services.Shutdown();
    std::filesystem::remove(file);
}

TEST(HlodRuntimeReconcile, MovedMemberFallsBackToMembersOnlyAndStaysStale) {
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));
    auto world = std::make_unique<ECS::World>(nullptr);
    ModelBank bank;
    Hlod::HlodRuntime& runtime = rs.GetHlodRuntime();

    const GUID matGuid = GUID::Derive(GUID::Null(), "reconcile/material3");
    const std::filesystem::path out =
        BakeAndReconcile(rs, *world, bank, runtime, matGuid, 0.0f, "hlod_reconcile_stale.gehlod");
    ASSERT_EQ(runtime.GetClusters().size(), 1u);

    Rendering::CameraId cam = rs.Views().AllocateCamera("HlodStaleCam");
    rs.Views().SetViewpointCamera(cam);
    HLODSelectSystem select(&rs);
    constexpr float kDt = 1.0f / 60.0f;

    // Far -> proxy-active.
    rs.Views().SetCameraData(cam, ::GameEngine::Tests::TestCameraWithEye(CameraAt(0.0f, 0.0f, -2000.0f)), kCameraAtRanges);
    select.Update(*world, kDt);
    ASSERT_TRUE(runtime.GetClusters()[0].ProxyActive);
    (void)rs.ConsumeResidencyExtractionPending();

    // Move one member (bump its WorldTransform.Version, as an editor edit would):
    // the cluster's frozen proxy no longer represents it (C3 / §3.4).
    const ECS::EntityHandle moved = runtime.GetClusters()[0].Members.front();
    auto* wt = world->GetComponentForWrite<WorldTransform>(moved);
    ASSERT_NE(wt, nullptr);
    wt->Position.X += 50.0;
    wt->Version += 1u;

    // Next select tick: the edit is detected -> cluster stale -> members-only.
    rs.Views().SetCameraData(cam, ::GameEngine::Tests::TestCameraWithEye(CameraAt(0.0f, 0.0f, -2000.0f)), kCameraAtRanges);
    select.Update(*world, kDt);

    EXPECT_TRUE(runtime.GetClusters()[0].Stale) << "a moved member must mark the cluster stale";
    EXPECT_FALSE(runtime.GetClusters()[0].ProxyActive) << "stale cluster falls back to members-only";
    for (ECS::EntityHandle member : runtime.GetClusters()[0].Members) {
        const auto* gpu = world->GetComponent<MeshGPUData>(member);
        ASSERT_NE(gpu, nullptr);
        EXPECT_FALSE(gpu->hlodEvicted) << "stale cluster: members resident";
    }
    for (ECS::EntityHandle proxy : runtime.GetClusters()[0].Proxies) {
        const auto* gpu = world->GetComponent<MeshGPUData>(proxy);
        ASSERT_NE(gpu, nullptr);
        EXPECT_TRUE(gpu->hlodEvicted) << "stale cluster: proxy evicted";
    }

    // Staleness is persistent: even a far camera never re-engages the proxy.
    (void)rs.ConsumeResidencyExtractionPending();
    rs.Views().SetCameraData(cam, ::GameEngine::Tests::TestCameraWithEye(CameraAt(0.0f, 0.0f, -3000.0f)), kCameraAtRanges);
    select.Update(*world, kDt);
    EXPECT_TRUE(runtime.GetClusters()[0].Stale);
    EXPECT_FALSE(runtime.GetClusters()[0].ProxyActive) << "stale is sticky until rebake + reload";

    runtime.Retire(*world, rs.GetMeshGPURegistry());
    world.reset();
    rs.Shutdown();
    std::error_code ec;
    std::filesystem::remove(out, ec);
}

TEST(HlodRuntimeReconcile, DestroyedParentedMemberMakesClusterStaleInStillScene) {
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    ExpectDestroyedMemberMakesClusterStale(device.get(), true, "hlod_reconcile_destroyed_parented.gehlod");
}

TEST(HlodRuntimeReconcile, DestroyedRootMemberMakesClusterStaleInStillScene) {
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    ExpectDestroyedMemberMakesClusterStale(device.get(), false, "hlod_reconcile_destroyed_root.gehlod");
}
