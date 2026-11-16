// Morph deep-path coverage (fusion T17, S2a review finding 5): the stamp
// audit's morph entity early-returns on a null model GUID, so the six
// value-gated write sites (four meshGpuHandleId assignments + two bounds
// writes) and their first-write semantics had zero automated coverage. This
// harness drives MorphTargetSystem against an in-memory synthesized morph
// ModelAsset via the resolver seam and locks: first-frame writes happen,
// idle frames take no grants, the first weight edit registers the runtime
// mesh and re-stamps, later edits rewrite it in place under the same handle,
// and zero weights hand the entity back to its source mesh.

#include <gtest/gtest.h>

#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "ECSModules/Rendering/Systems/MorphTargetSystem.h"

#include "ECS/World.h"
#include "ECS/Entity.h"
#include "ECS/Query.h"
#include "ECS/ChangeFilter.h"
#include "ECS/ECSTemplates.h"

#include "Components/Rendering/LocalBounds.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Rendering/MorphTargetWeights.h"

#include "Assets/ModelAsset.h"
#include "AssetCore/GUID.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/GPUScene.h"

#include <filesystem>
#include <memory>
#include <unordered_map>
#include <vector>

using namespace GameEngine;
using GameEngine::Engine::Renderer::MorphTargetSystem;
using GameEngine::Engine::Renderer::RenderServices;

#include "TestDeviceHelper.h"

namespace
{

constexpr float32 kDt = 1.0f / 60.0f;

// Entities visited by a Changed<C>-gated scan since `gate` — the stamp-audit
// probe shape (visiting form; the const binding keeps the probe read-only).
template <typename C>
size_t VisitedSince(ECS::World& world, uint64 gate)
{
    ECS::ChangeGate g;
    g.LastRunVersion = gate;
    size_t visited = 0;
    auto q = world.Query<ECS::Read<C>>();
    q.template Changed<C>(g);
    q.BatchEach([&](const C*, std::size_t count) { visited += count; });
    return visited;
}

// A one-mesh model with a single morph target that pushes vertex 0 up.
Mesh MakeMorphSourceMesh()
{
    Mesh m{};
    m.Name = "MorphDeepPathTriangle";
    Vertex v0{}, v1{}, v2{};
    v0.Position[1] = 1.0f;
    v1.Position[0] = -1.0f;
    v1.Position[1] = -1.0f;
    v2.Position[0] = 1.0f;
    v2.Position[1] = -1.0f;
    v0.Normal[2] = v1.Normal[2] = v2.Normal[2] = 1.0f;
    m.Vertices = {v0, v1, v2};
    m.Indices = {0, 1, 2};
    m.MinBounds[0] = -1.0f;
    m.MinBounds[1] = -1.0f;
    m.MinBounds[2] = 0.0f;
    m.MaxBounds[0] = 1.0f;
    m.MaxBounds[1] = 1.0f;
    m.MaxBounds[2] = 0.0f;

    MorphTarget target{};
    target.Name = "raise";
    target.VertexIndices = {0u};
    target.PositionDeltas = {0.0f, 2.0f, 0.0f};  // vertex 0 up
    m.MorphTargets.push_back(std::move(target));
    return m;
}

} // namespace

TEST(MorphTargetDeepPathTests, SynthesizedModelDrivesGatedWritesAndReRegisters)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));

    const GUID modelGuid = GUID::Generate();
    auto model = std::make_unique<ModelAsset>(modelGuid,
                                              std::filesystem::path("Synthetic://MorphDeepPath"));
    Vector<Mesh> meshes;
    meshes.push_back(MakeMorphSourceMesh());
    model->SetMeshesForTest(std::move(meshes));
    ASSERT_TRUE(model->IsLoaded());

    MorphTargetSystem morphSystem(&rs);
    morphSystem.SetModelResolverForTest(
        [&](const GUID& guid) { return guid == modelGuid ? model.get() : nullptr; });

    ECS::World world(nullptr);
    ECS::Entity e = world.Create();
    Components::MeshRenderer mr{};
    mr.modelAssetGuid.Set(modelGuid);
    mr.meshId = 0;
    e.Set(mr);
    e.Set(Components::MorphTargetWeights{});
    e.Set(Components::LocalBounds{});
    world.ProcessCommands();

    size_t unregisterNotifications = 0;
    auto token = rs.GetMeshGPURegistry().SubscribeReload(
        [&](const GUID&) { ++unregisterNotifications; });

    // First frame: zero weights -> the SOURCE mesh is registered and the
    // first gated writes fire (handle assignment + bounds from the entry).
    morphSystem.Update(world, kDt);
    const auto* mrRead = world.GetComponent<Components::MeshRenderer>(e.GetHandle());
    ASSERT_NE(mrRead, nullptr);
    ASSERT_NE(mrRead->meshGpuHandleId, 0u) << "first-frame source handle write missing";
    const uint64 sourceHandle = mrRead->meshGpuHandleId;
    const auto* lb = world.GetComponent<Components::LocalBounds>(e.GetHandle());
    ASSERT_NE(lb, nullptr);
    EXPECT_FLOAT_EQ(lb->Box.halfExtents.y, 1.0f)
        << "first-frame bounds write must reflect the source mesh bounds";
    const auto* weights = world.GetComponent<Components::MorphTargetWeights>(e.GetHandle());
    ASSERT_NE(weights, nullptr);
    EXPECT_EQ(weights->weightCount, 1u);
    EXPECT_EQ(weights->appliedVersion, weights->version);

    // Idle frames: the value gates must suppress every grant on the probed
    // columns — this is the deep-path half the stamp audit could not reach.
    const uint64 gate = world.GetGlobalSystemVersion();
    for (int i = 0; i < 8; ++i)
        morphSystem.Update(world, kDt);
    EXPECT_EQ(VisitedSince<Components::MeshRenderer>(world, gate), 0u)
        << "an idle deep-path frame write-granted MeshRenderer";
    EXPECT_EQ(VisitedSince<Components::LocalBounds>(world, gate), 0u)
        << "an idle deep-path frame write-granted LocalBounds";

    // Weight edit: the runtime morphed mesh is registered and assigned.
    if (auto* w = world.GetComponentForWrite<Components::MorphTargetWeights>(e.GetHandle()))
    {
        w->weights[0] = 1.0f;
        ++w->version;
    }
    morphSystem.Update(world, kDt);
    mrRead = world.GetComponent<Components::MeshRenderer>(e.GetHandle());
    ASSERT_NE(mrRead, nullptr);
    EXPECT_NE(mrRead->meshGpuHandleId, 0u);
    EXPECT_NE(mrRead->meshGpuHandleId, sourceHandle)
        << "non-zero weights must swap to the runtime morphed mesh";
    weights = world.GetComponent<Components::MorphTargetWeights>(e.GetHandle());
    ASSERT_NE(weights, nullptr);
    EXPECT_EQ(weights->appliedVersion, weights->version);
    EXPECT_FLOAT_EQ(weights->appliedWeights[0], 1.0f);
    const uint64 firstRuntimeHandle = mrRead->meshGpuHandleId;

    // Second weight edit: the runtime mesh's vertices are rewritten in place,
    // so the handle stays and no row is freed (no E6 notification).
    const size_t notificationsBefore = unregisterNotifications;
    if (auto* w = world.GetComponentForWrite<Components::MorphTargetWeights>(e.GetHandle()))
    {
        w->weights[0] = 0.5f;
        ++w->version;
    }
    morphSystem.Update(world, kDt);
    EXPECT_EQ(unregisterNotifications, notificationsBefore)
        << "an in-place rewrite must not announce a freed row";
    mrRead = world.GetComponent<Components::MeshRenderer>(e.GetHandle());
    ASSERT_NE(mrRead, nullptr);
    EXPECT_EQ(mrRead->meshGpuHandleId, firstRuntimeHandle)
        << "a weight edit must keep the runtime mesh handle";
    weights = world.GetComponent<Components::MorphTargetWeights>(e.GetHandle());
    ASSERT_NE(weights, nullptr);
    EXPECT_FLOAT_EQ(weights->appliedWeights[0], 0.5f);

    // Weights back to zero: the runtime mesh is dropped and the source
    // handle is re-assigned.
    if (auto* w = world.GetComponentForWrite<Components::MorphTargetWeights>(e.GetHandle()))
    {
        w->weights[0] = 0.0f;
        ++w->version;
    }
    morphSystem.Update(world, kDt);
    mrRead = world.GetComponent<Components::MeshRenderer>(e.GetHandle());
    ASSERT_NE(mrRead, nullptr);
    EXPECT_EQ(mrRead->meshGpuHandleId, sourceHandle)
        << "zero weights must fall back to the source mesh handle";
    weights = world.GetComponent<Components::MorphTargetWeights>(e.GetHandle());
    ASSERT_NE(weights, nullptr);
    EXPECT_EQ(weights->runtimeMeshGpuHandleId, 0u);

    token.Reset();
    rs.Shutdown();
    device->Shutdown();
}

// A steady weight animation rewrites the runtime mesh's vertices in place: the
// handle, the GPUScene row and the index range stay, no unregister event fires,
// and a vertex range becomes current again only once every frame that could
// still read it has completed.
TEST(MorphTargetDeepPathTests, WeightAnimationRewritesTheRuntimeMeshInPlace)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));
    auto& registry = rs.GetMeshGPURegistry();

    const GUID modelGuid = GUID::Generate();
    auto model = std::make_unique<ModelAsset>(modelGuid,
                                              std::filesystem::path("Synthetic://MorphInPlace"));
    Vector<Mesh> meshes;
    meshes.push_back(MakeMorphSourceMesh());
    model->SetMeshesForTest(std::move(meshes));
    ASSERT_TRUE(model->IsLoaded());

    MorphTargetSystem morphSystem(&rs);
    morphSystem.SetModelResolverForTest(
        [&](const GUID& guid) { return guid == modelGuid ? model.get() : nullptr; });

    ECS::World world(nullptr);
    ECS::Entity e = world.Create();
    Components::MeshRenderer mr{};
    mr.modelAssetGuid.Set(modelGuid);
    mr.meshId = 0;
    e.Set(mr);
    e.Set(Components::MorphTargetWeights{});
    e.Set(Components::LocalBounds{});
    world.ProcessCommands();

    uint64_t frame = 0;
    const auto stepWithWeight = [&](float weight)
    {
        registry.BeginFrame(++frame);
        if (auto* w = world.GetComponentForWrite<Components::MorphTargetWeights>(e.GetHandle()))
        {
            w->weights[0] = weight;
            ++w->version;
        }
        morphSystem.Update(world, kDt);
    };

    stepWithWeight(0.1f);
    const auto* weights = world.GetComponent<Components::MorphTargetWeights>(e.GetHandle());
    ASSERT_NE(weights, nullptr);
    const uint64 runtimeHandleId = weights->runtimeMeshGpuHandleId;
    ASSERT_NE(runtimeHandleId, 0u) << "setup: a runtime morphed mesh must be registered";
    const Rendering::MeshGPUHandle runtimeHandle(runtimeHandleId);
    const Rendering::MeshGPUEntry* entry = registry.Find(runtimeHandle);
    ASSERT_NE(entry, nullptr);
    ASSERT_NE(entry->gpuMeshIndex, ~0u) << "setup: the runtime mesh must own a GPUScene row";
    const uint32_t gpuMeshIndex = entry->gpuMeshIndex;
    const uint32_t firstIndex = entry->firstIndex;
    const uint64_t indexOffsetBytes = entry->indexSubAlloc.Offset;

    size_t unregisterNotifications = 0;
    auto token = registry.SubscribeReload([&](const GUID&) { ++unregisterNotifications; });

    // The frame each vertex offset was last drawn from. A range the entry moves
    // back to must have stayed out of use for a full frames-in-flight window.
    std::unordered_map<uint32_t, uint64_t> lastCurrentFrame{{entry->vertexOffset, frame}};
    uint32_t currentVertexOffset = entry->vertexOffset;
    for (int step = 1; step <= 16; ++step)
    {
        const float weight = 0.1f + 0.05f * static_cast<float>(step);
        stepWithWeight(weight);

        weights = world.GetComponent<Components::MorphTargetWeights>(e.GetHandle());
        ASSERT_NE(weights, nullptr);
        EXPECT_EQ(weights->runtimeMeshGpuHandleId, runtimeHandleId)
            << "a weight change must keep the runtime mesh handle (step " << step << ")";
        const auto* mrRead = world.GetComponent<Components::MeshRenderer>(e.GetHandle());
        ASSERT_NE(mrRead, nullptr);
        EXPECT_EQ(mrRead->meshGpuHandleId, runtimeHandleId);

        entry = registry.Find(runtimeHandle);
        ASSERT_NE(entry, nullptr) << "the runtime mesh entry was released (step " << step << ")";
        EXPECT_EQ(entry->gpuMeshIndex, gpuMeshIndex) << "the GPUScene row moved";
        EXPECT_EQ(entry->firstIndex, firstIndex) << "the index data was uploaded again";
        EXPECT_EQ(entry->indexSubAlloc.Offset, indexOffsetBytes);
        // Vertex 0 rises by 2 * weight, so the box spans y in [-1, 1 + 2w].
        EXPECT_NEAR(entry->bounds.halfExtents.y, 1.0f + weight, 1e-5f)
            << "the rewritten vertices did not reach the entry bounds";

        if (entry->vertexOffset != currentVertexOffset)
        {
            const auto previous = lastCurrentFrame.find(entry->vertexOffset);
            if (previous != lastCurrentFrame.end())
            {
                EXPECT_GE(frame - previous->second,
                          static_cast<uint64_t>(Rendering::IDevice::kMaxSupportedFramesInFlight))
                    << "vertex offset " << entry->vertexOffset
                    << " was rewritten while a frame in flight could still read it";
            }
            currentVertexOffset = entry->vertexOffset;
        }
        lastCurrentFrame[currentVertexOffset] = frame;
    }
    EXPECT_EQ(unregisterNotifications, 0u)
        << "a weight change must not free and re-create the runtime mesh row";

    token.Reset();
    rs.Shutdown();
    device->Shutdown();
}
