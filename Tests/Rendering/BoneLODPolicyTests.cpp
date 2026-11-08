#include "Engine/Rendering/BoneLOD/BoneLODPolicy.h"
#include "Animation/BoneEvaluationPlan.h"
#include "Assets/AnimationClip.h"
#include "Assets/AssetManager.h"
#include "Assets/BoneLOD/BoneLODAssetSettings.h"
#include "Assets/BoneLOD/ModelBoneLOD.h"
#include "Assets/ModelAsset.h"
#include "Assets/ModelAssetSettings.h"
#include "Components/Animation/AnimatedNodeRef.h"
#include "Components/Animation/Animator.h"
#include "Components/Animation/AnimatorRef.h"
#include "Components/Animation/SkeletonRef.h"
#include "Components/Rendering/LocalBounds.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Transform.h"
#include "Core/Application.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "ECSModules/Rendering/ClipStore.h"
#include "ECSModules/Rendering/SkeletonStore.h"
#include "ECSModules/Rendering/Systems/AnimationSystem.h"
#include "Engine/Rendering/RenderServices.h"
#include "Mathematics/Matrix4x4.h"
#include "Mathematics/MatrixOps.h"
#include "Scripting/ScriptManager.h"
#include "Scene/SceneIO.h"
#include "TestTempDir.h"

#include <algorithm>
#include <fstream>
#include <numbers>

#include <gtest/gtest.h>

using namespace GameEngine;
namespace Renderer = GameEngine::Engine::Renderer;

namespace
{

void InitializeIdentityRest(Animation::SkeletonData& skeleton)
{
    skeleton.RestTranslation.assign(skeleton.BoneCount * 3u, 0.0f);
    skeleton.RestRotation.assign(skeleton.BoneCount * 4u, 0.0f);
    skeleton.RestScale.assign(skeleton.BoneCount * 3u, 1.0f);
    skeleton.RestLocalMatrix.resize(skeleton.BoneCount * 16u);
    skeleton.InverseBind.resize(skeleton.BoneCount * 16u);
    const Mathematics::Matrix4x4 identity;
    for (uint32 node = 0u; node < skeleton.BoneCount; ++node)
    {
        skeleton.RestRotation[node * 4u + 3u] = 1.0f;
        std::copy_n(identity.Data(), 16u, skeleton.RestLocalMatrix.data() + node * 16u);
        std::copy_n(identity.Data(), 16u, skeleton.InverseBind.data() + node * 16u);
    }
}

class BoneLODPolicyTest : public testing::Test
{
protected:
    void SetUp() override
    {
        std::filesystem::create_directories(AssetsPath);
        std::ofstream(ModelPath) << "metadata fixture";
        ApplicationConfig config;
        config.WorkspaceDirectory = Temporary.Path().string();
        config.AssetDirectory = AssetsPath.string();
        config.AssetDatabaseFile = (Temporary.Path() / "AssetDatabase.assetdb").string();
        config.AssetDatabaseCacheDirectory = (Temporary.Path() / ".Cache").string();
        ScriptsConfig scripts;
        scripts.disableClr = true;
        scripts.enableAutoProjectGeneration = false;
        EngineCore::GetInstance().SetScriptsConfig(scripts);
        ASSERT_TRUE(EngineCore::GetInstance().Initialize(config));
        Assets = EngineCore::GetInstance().TryGetAssetManager();
        ASSERT_NE(Assets, nullptr);
        Assets->WaitForStartupScan();
        ModelGuid = Assets->ResolveAssetGuid(ModelPath);
        ASSERT_FALSE(ModelGuid.IsNull());
        ModelAssetSettings rigSettings;
        rigSettings.RigKind = ModelRigKind::NotHumanoid;
        ASSERT_TRUE(rigSettings.Save(Assets->GetRegistry(), ModelPath));
        BoneLODAssetSettings settings;
        settings.BoneBudget = 2u;
        ASSERT_TRUE(settings.Save(Assets->GetRegistry(), ModelPath));

        auto& store = Renderer::SkeletonStore::Instance();
        SkeletonId = store.CreateSkeleton(4u);
        auto* skeleton = store.Get(SkeletonId);
        skeleton->Parent = {-1, 0, 0, 0};
        skeleton->BoneNames = {"Root", "Kept", "Omitted", "Other"};
        skeleton->BuildBoneNameLookup();
        InitializeIdentityRest(*skeleton);
        skeleton->ComputeTopologicalSort();
        Mesh mesh;
        mesh.Skinned = true;
        mesh.Vertices.resize(2u);
        mesh.Vertices[0].Position[0] = 0.3f;
        mesh.Vertices[1].Position[0] = 0.1f;
        mesh.Joints0 = {1u, 0u, 0u, 0u, 2u, 0u, 0u, 0u};
        mesh.Weights0 = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f};
        Model = MakeShared<ModelAsset>(ModelGuid, ModelPath);
        Model->SetSkeletonIdForTest(SkeletonId);
        Model->SetMeshesForTest({mesh});
        {
            AssetManager::ScopedThreadAssetManager loading(Assets);
            Model->PostLoad();
        }
        ASSERT_TRUE(Model->GetBoneLOD());
        ASSERT_TRUE(Model->GetBoneLOD()->ReducedPlan());
        Assets->RegisterLoadedAsset(ModelGuid, Model);
        ClipIndex = RegisterClip(4.0f);
        Owner = World.CreateEntity();
        World.AddComponentImmediate(Owner, Components::BoneLOD{});
        World.AddComponentImmediate(Owner, Components::Animator{});
        RuntimeId = CreateRuntime(World, Owner);
        auto reference = Reference(Owner, RuntimeId);
        reference.ownerMode = Components::SkeletonInstanceOwner::Self;
        reference.instanceOwner = {};
        World.AddComponentImmediate(Owner, reference);
        Body = AddConsumer(World, Owner, RuntimeId, true);
        Camera = Services.Views().AllocateCamera("BoneLOD.Test");
        View = Services.Views().AllocateView("BoneLOD.Test", Camera);
        SetCamera(Camera, 1000.0f);
        System = MakeUnique<Renderer::AnimationSystem>(&Services);
    }

    void TearDown() override
    {
        System.reset();
        World.Clear();
        auto& store = Renderer::SkeletonStore::Instance();
        for (const auto& [id, generation] : OwnedRuntimes)
        {
            while (store.GetRuntimeGeneration(id) == generation && store.GetRuntimeRefCount(id) > 0u)
                store.ReleaseRuntime(id);
        }
        Services.Shutdown();
        EngineCore::GetInstance().Shutdown();
        std::filesystem::current_path(PreviousDirectory);
    }

    uint32 RegisterClip(float endX)
    {
        const GUID guid = GUID::Generate();
        auto clip = MakeShared<AnimationClip>(guid, "Synthetic://BoneLODClip");
        AnimChannel channel;
        channel.boneIndex = 2u;
        channel.path = AnimPath::Translation;
        channel.interp = AnimInterp::Linear;
        AnimKeyframe first{};
        AnimKeyframe last{};
        last.time = 1.0f;
        last.translation[0] = endX;
        channel.keys = {first, last};
        clip->SetChannelsAndDurationForTest({channel}, 1.0f);
        return Renderer::ClipStore::Instance().RegisterRuntimeClip(guid, std::move(clip));
    }

    uint32 CreateRuntime(ECS::World& world, ECS::EntityHandle owner)
    {
        auto& store = Renderer::SkeletonStore::Instance();
        const uint32 id = store.CreateRuntime(SkeletonId);
        auto* runtime = store.GetRuntime(id);
        runtime->Source = {ModelGuid, owner, world.GetWorldId(), world.GetLifecycleResetGeneration()};
        OwnedRuntimes.emplace_back(id, store.GetRuntimeGeneration(id));
        return id;
    }

    Components::SkeletonRef Reference(ECS::EntityHandle owner, uint32 runtimeId) const
    {
        Components::SkeletonRef reference;
        reference.skeletonId = SkeletonId;
        reference.runtimeId = runtimeId;
        reference.runtimeGeneration = Renderer::SkeletonStore::Instance().GetRuntimeGeneration(runtimeId);
        reference.sourceModelGuid.Set(ModelGuid);
        reference.instanceOwner = owner;
        reference.ownerMode = Components::SkeletonInstanceOwner::Entity;
        return reference;
    }

    ECS::EntityHandle AddConsumer(ECS::World& world, ECS::EntityHandle owner, uint32 runtimeId, bool retain)
    {
        if (retain)
            Renderer::SkeletonStore::Instance().RetainRuntime(runtimeId);
        const auto entity = world.CreateEntity();
        world.AddComponentImmediate(entity, Reference(owner, runtimeId));
        Components::MeshRenderer mesh;
        mesh.modelAssetGuid.Set(ModelGuid);
        world.AddComponentImmediate(entity, mesh);
        Components::LocalBounds bounds;
        bounds.Box.halfExtents = Mathematics::Vector3(5.0f, 1.0f, 1.0f);
        world.AddComponentImmediate(entity, bounds);
        world.AddComponentImmediate(entity, Components::WorldTransform{});
        Components::AnimatorRef playback;
        playback.ClipIndex = ClipIndex;
        playback.Time = 0.5f;
        playback.Flags = Components::AnimatorRef::kFlag_Paused;
        world.AddComponentImmediate(entity, playback);
        return entity;
    }

    void SetCamera(Rendering::CameraId cameraId, float z)
    {
        Rendering::CameraData camera{};
        const auto projection = Mathematics::Matrix4x4::PerspectiveReverseZ(std::numbers::pi_v<float> * 0.5f,
            1.0f, 0.1f, 2000.0f);
        std::copy_n(projection.Data(), 16u, camera.proj);
        camera.cameraPos[2] = z;
        Services.Views().SetCameraData(cameraId, camera);
    }

    void UpdatePolicy() { Policy.Update(World, &Services.Views(), Assets, 0.0f); }
    bool Reduced() const
    {
        const auto* selection = Policy.Find(RuntimeId);
        return selection && selection->Reduced;
    }
    float OmittedTranslation() const
    {
        const auto* runtime = Renderer::SkeletonStore::Instance().GetRuntime(RuntimeId);
        return runtime->CompactSkinMatrices[2u * 16u + 12u];
    }

    TestUtils::ScopedTempDir Temporary{TestUtils::MakeUniqueTempDirectory("bone_lod_policy")};
    const std::filesystem::path PreviousDirectory = std::filesystem::current_path();
    const std::filesystem::path AssetsPath = Temporary.Path() / "Assets";
    const std::filesystem::path ModelPath = AssetsPath / "Character.fbx";
    AssetManager* Assets = nullptr;
    SharedPtr<ModelAsset> Model;
    GUID ModelGuid;
    ECS::World World;
    Renderer::RenderServices Services;
    Renderer::BoneLODPolicy Policy;
    UniquePtr<Renderer::AnimationSystem> System;
    std::vector<std::pair<uint32, uint64>> OwnedRuntimes;
    ECS::EntityHandle Owner;
    ECS::EntityHandle Body;
    uint32 SkeletonId = 0u;
    uint32 RuntimeId = 0u;
    uint32 ClipIndex = 0u;
    Rendering::CameraId Camera = 0u;
    Rendering::ViewId View = 0u;
};

} // namespace

TEST_F(BoneLODPolicyTest, AggregatesViewsAndRemovesRetiredViewDemand)
{
    UpdatePolicy();
    ASSERT_TRUE(Reduced());
    const auto camera = Services.Views().AllocateCamera("BoneLOD.Second");
    const auto view = Services.Views().AllocateView("BoneLOD.Second", camera);
    SetCamera(camera, 10.0f);
    UpdatePolicy();
    EXPECT_FALSE(Reduced());
    ASSERT_TRUE(Services.Views().ReleaseView(view));
    UpdatePolicy();
    EXPECT_TRUE(Reduced());
}

TEST_F(BoneLODPolicyTest, ConflictingConsumersUseMaximumNormalizedDemand)
{
    Components::BoneLOD first;
    first.SwitchCoverage = 0.004f;
    World.AddComponentImmediate(Body, first);
    const auto sibling = AddConsumer(World, Owner, RuntimeId, true);
    Components::BoneLOD second;
    second.SwitchCoverage = 0.2f;
    World.AddComponentImmediate(sibling, second);
    World.GetComponentForWrite<Components::WorldTransform>(sibling)->matrix[14] = 950.0f;
    UpdatePolicy();
    EXPECT_FALSE(Reduced());
}

TEST_F(BoneLODPolicyTest, UnknownAndForeignMeshSourcesForceTheSharedRuntimeFull)
{
    UpdatePolicy();
    ASSERT_TRUE(Reduced());
    for (const auto& guid : {GUID::Generate(), GUID{}})
    {
        World.GetComponentForWrite<Components::MeshRenderer>(Body)->modelAssetGuid.Set(guid);
        UpdatePolicy();
        EXPECT_FALSE(Reduced());
        World.GetComponentForWrite<Components::MeshRenderer>(Body)->modelAssetGuid.Set(ModelGuid);
        UpdatePolicy();
        EXPECT_TRUE(Reduced());
    }
}

TEST_F(BoneLODPolicyTest, MatchingImportedHelperAllowsReductionButMismatchedSourceDoesNot)
{
    const auto helper = World.CreateEntity();
    ASSERT_TRUE(Renderer::SkeletonStore::Instance().RetainRuntime(RuntimeId));
    World.AddComponentImmediate(helper, Reference(Owner, RuntimeId));
    UpdatePolicy();
    EXPECT_TRUE(Reduced());
    World.GetComponentForWrite<Components::SkeletonRef>(helper)->sourceModelGuid.Set(GUID::Generate());
    UpdatePolicy();
    EXPECT_FALSE(Reduced());
}

TEST_F(BoneLODPolicyTest, OwnerFullDetailOverrideWinsEvenWithChildSettings)
{
    World.AddComponentImmediate(Body, Components::BoneLOD{});
    World.GetComponentForWrite<Components::BoneLOD>(Owner)->FullDetailOverride = true;
    UpdatePolicy();
    EXPECT_FALSE(Reduced());
}

TEST_F(BoneLODPolicyTest, SharedHelperCanRequestFullDetailWithoutRendering)
{
    UpdatePolicy();
    ASSERT_TRUE(Reduced());
    const auto helper = World.CreateEntity();
    ASSERT_TRUE(Renderer::SkeletonStore::Instance().RetainRuntime(RuntimeId));
    World.AddComponentImmediate(helper, Reference(Owner, RuntimeId));
    Components::BoneLOD settings;
    settings.FullDetailOverride = true;
    World.AddComponentImmediate(helper, settings);
    UpdatePolicy();
    EXPECT_FALSE(Reduced());
}

TEST_F(BoneLODPolicyTest, CoverageHysteresisRetainsBothTiersInsideTheBand)
{
    SetCamera(Camera, 200.0f);
    UpdatePolicy();
    ASSERT_TRUE(Reduced());
    SetCamera(Camera, 95.0f);
    UpdatePolicy();
    EXPECT_TRUE(Reduced());
    SetCamera(Camera, 75.0f);
    UpdatePolicy();
    EXPECT_FALSE(Reduced());
    SetCamera(Camera, 95.0f);
    UpdatePolicy();
    EXPECT_FALSE(Reduced());
    SetCamera(Camera, 200.0f);
    UpdatePolicy();
    EXPECT_TRUE(Reduced());
}

TEST_F(BoneLODPolicyTest, OmittedAttachmentForcesSharedRuntimeFull)
{
    UpdatePolicy();
    ASSERT_TRUE(Reduced());
    const auto attachment = World.CreateEntity();
    Renderer::SkeletonStore::Instance().RetainRuntime(RuntimeId);
    World.AddComponentImmediate(attachment, Reference(Owner, RuntimeId));
    World.AddComponentImmediate(attachment, Components::AnimatedNodeRef{2u});
    UpdatePolicy();
    EXPECT_FALSE(Reduced());
}

TEST_F(BoneLODPolicyTest, MissingViewsAndDisabledLastOptInResetToFull)
{
    UpdatePolicy();
    ASSERT_TRUE(Reduced());
    ASSERT_TRUE(Services.Views().ReleaseView(View));
    UpdatePolicy();
    EXPECT_FALSE(Reduced());
    ECS::Entity(&World, Owner).SetEnabled<Components::BoneLOD>(false);
    UpdatePolicy();
    EXPECT_EQ(Policy.Find(RuntimeId), nullptr);
}

TEST_F(BoneLODPolicyTest, GraphOwnerAndStaleRuntimeGenerationCannotReduce)
{
    World.GetComponentForWrite<Components::Animator>(Owner)->source = Components::AnimatorPlaybackSource::Graph;
    UpdatePolicy();
    EXPECT_FALSE(Reduced());
    World.GetComponentForWrite<Components::Animator>(Owner)->source = Components::AnimatorPlaybackSource::Clip;
    auto* reference = World.GetComponentForWrite<Components::SkeletonRef>(Body);
    --reference->runtimeGeneration;
    UpdatePolicy();
    EXPECT_FALSE(Reduced());
}

TEST_F(BoneLODPolicyTest, ReadOnlySelectionDoesNotWriteEcsColumns)
{
    const auto stamps = World.GetColumnStampCount();
    UpdatePolicy();
    UpdatePolicy();
    EXPECT_EQ(World.GetColumnStampCount(), stamps);
}

TEST_F(BoneLODPolicyTest, StaleReferenceCannotAnimateARecycledRuntimeSlot)
{
    auto& store = Renderer::SkeletonStore::Instance();
    const uint64 oldGeneration = store.GetRuntimeGeneration(RuntimeId);
    while (store.GetRuntimeRefCount(RuntimeId) > 0u)
        store.ReleaseRuntime(RuntimeId);
    const auto replacementOwner = World.CreateEntity();
    const uint32 replacementId = CreateRuntime(World, replacementOwner);
    ASSERT_EQ(replacementId, RuntimeId);
    ASSERT_NE(store.GetRuntimeGeneration(replacementId), oldGeneration);
    auto* replacement = store.GetRuntime(replacementId);
    ASSERT_NE(replacement, nullptr);
    std::fill(replacement->CompactSkinMatrices.begin(), replacement->CompactSkinMatrices.end(), 77.0f);
    auto* playback = World.GetComponentForWrite<Components::AnimatorRef>(Body);
    playback->Flags = 0u;
    System->Update(World, 0.1f);
    EXPECT_TRUE(std::all_of(replacement->CompactSkinMatrices.begin(), replacement->CompactSkinMatrices.end(),
        [](float value) { return value == 77.0f; }));
    EXPECT_FLOAT_EQ(World.GetComponent<Components::AnimatorRef>(Body)->Time, 0.5f);
}

TEST_F(BoneLODPolicyTest, IncompatibleSkeletonReferenceCannotWriteTheRuntimePalette)
{
    auto& store = Renderer::SkeletonStore::Instance();
    const uint32 foreignSkeleton = store.CreateSkeleton(4u);
    World.GetComponentForWrite<Components::SkeletonRef>(Body)->skeletonId = foreignSkeleton;
    auto* runtime = store.GetRuntime(RuntimeId);
    std::fill(runtime->CompactSkinMatrices.begin(), runtime->CompactSkinMatrices.end(), 77.0f);
    System->Update(World, 0.1f);
    EXPECT_TRUE(std::all_of(runtime->CompactSkinMatrices.begin(), runtime->CompactSkinMatrices.end(),
        [](float value) { return value == 77.0f; }));
}

TEST_F(BoneLODPolicyTest, SequentialWorldUpdatesDoNotRetainAnotherWorldSelection)
{
    UpdatePolicy();
    ASSERT_TRUE(Reduced());
    ECS::World other;
    const auto owner = other.CreateEntity();
    other.AddComponentImmediate(owner, Components::BoneLOD{});
    const uint32 runtime = CreateRuntime(other, owner);
    AddConsumer(other, owner, runtime, false);
    Policy.Update(other, &Services.Views(), Assets, 0.0f);
    EXPECT_EQ(Policy.Find(RuntimeId), nullptr);
    ASSERT_NE(Policy.Find(runtime), nullptr);
    EXPECT_TRUE(Policy.Find(runtime)->Reduced);
    UpdatePolicy();
    EXPECT_EQ(Policy.Find(runtime), nullptr);
    EXPECT_TRUE(Reduced());
}

TEST_F(BoneLODPolicyTest, FrozenPlaybackChangesTierAndInvalidatesShadowContent)
{
    System->Update(World, 0.1f);
    EXPECT_FLOAT_EQ(OmittedTranslation(), 0.0f);
    auto version = Services.ShadowCasterContentVersion(World.GetWorldId());
    System->Update(World, 0.1f);
    EXPECT_EQ(Services.ShadowCasterContentVersion(World.GetWorldId()), version);
    SetCamera(Camera, 10.0f);
    System->Update(World, 0.1f);
    EXPECT_FLOAT_EQ(OmittedTranslation(), 2.0f);
    EXPECT_GT(Services.ShadowCasterContentVersion(World.GetWorldId()), version);
    version = Services.ShadowCasterContentVersion(World.GetWorldId());
    System->Update(World, 0.1f);
    EXPECT_EQ(Services.ShadowCasterContentVersion(World.GetWorldId()), version);
    SetCamera(Camera, 1000.0f);
    System->Update(World, 0.1f);
    EXPECT_FLOAT_EQ(OmittedTranslation(), 0.0f);
    EXPECT_GT(Services.ShadowCasterContentVersion(World.GetWorldId()), version);
    EXPECT_FLOAT_EQ(World.GetComponent<Components::AnimatorRef>(Body)->Time, 0.5f);
}

TEST_F(BoneLODPolicyTest, CrossfadeUsesTheSamePlanAndFullDetailOverride)
{
    auto* playback = World.GetComponentForWrite<Components::AnimatorRef>(Body);
    playback->PrevClipIndex = RegisterClip(-4.0f);
    playback->PrevTime = 0.5f;
    playback->BlendDuration = 1.0f;
    playback->BlendTime = 0.25f;
    System->Update(World, 0.1f);
    EXPECT_FLOAT_EQ(OmittedTranslation(), 0.0f);
    World.GetComponentForWrite<Components::BoneLOD>(Owner)->FullDetailOverride = true;
    System->Update(World, 0.1f);
    EXPECT_FLOAT_EQ(OmittedTranslation(), -1.0f);
}

TEST_F(BoneLODPolicyTest, PausedModelAdoptionRefreshesSelectionAndContentIdentity)
{
    System->Update(World, 0.1f);
    EXPECT_FLOAT_EQ(OmittedTranslation(), 0.0f);
    const auto version = Services.ShadowCasterContentVersion(World.GetWorldId());
    BoneLODAssetSettings settings;
    settings.BoneBudget = 2u;
    settings.MandatoryPaths = {{"Root", "Omitted"}};
    ASSERT_TRUE(settings.Save(Assets->GetRegistry(), ModelPath));
    ModelAsset staged(ModelGuid, ModelPath);
    staged.SetSkeletonIdForTest(SkeletonId);
    staged.SetMeshesForTest(Model->GetMeshes());
    {
        AssetManager::ScopedThreadAssetManager loading(Assets);
        staged.PostLoad();
    }
    ASSERT_TRUE(Model->AdoptReload(staged));
    System->Update(World, 0.1f);
    EXPECT_FLOAT_EQ(OmittedTranslation(), 2.0f);
    EXPECT_GT(Services.ShadowCasterContentVersion(World.GetWorldId()), version);
}

TEST_F(BoneLODPolicyTest, KernelLimitCacheDoesNotPoisonFullToReducedRouting)
{
    auto& store = Renderer::SkeletonStore::Instance();
    const uint32 id = store.CreateSkeleton(300u);
    auto* skeleton = store.Get(id);
    skeleton->Parent.assign(300u, 0);
    skeleton->Parent[0] = -1;
    InitializeIdentityRest(*skeleton);
    skeleton->ComputeTopologicalSort();
    auto plan = MakeShared<Animation::BoneEvaluationPlan>();
    ASSERT_TRUE(plan->Build(*skeleton, {}));
    Renderer::AnimationSystem probe(nullptr);
    auto& clips = Renderer::ClipStore::Instance();
    EXPECT_FALSE(probe.IsClipSkeletonGPUSafe(id, ClipIndex, *skeleton, clips));
    EXPECT_TRUE(probe.IsClipSkeletonGPUSafe(id, ClipIndex, *skeleton, clips, plan.get()));
    EXPECT_FALSE(probe.IsClipSkeletonGPUSafe(id, ClipIndex, *skeleton, clips));
}

TEST_F(BoneLODPolicyTest, SceneRoundTripPreservesTheInstanceOptInAndFullDetailOverride)
{
    ECS::World authored;
    const auto entity = authored.CreateEntity();
    Components::BoneLOD settings;
    settings.SwitchCoverage = 0.125f;
    settings.FullDetailOverride = true;
    authored.AddComponentImmediate(entity, settings);
    const auto file = Temporary.Path() / "BoneLOD.scene";
    ASSERT_TRUE(Scene::SaveSceneToFile(authored, file));
    ECS::World loaded;
    Scene::SceneLoadDegradation degradation;
    Scene::LoadOptions options;
    options.outDegradation = &degradation;
    ASSERT_TRUE(Scene::LoadSceneFromFile(loaded, file, options)) << Scene::GetLastSceneIOError().message;
    EXPECT_FALSE(degradation.IsDegraded());
    uint32 count = 0u;
    loaded.Query<ECS::Read<Components::BoneLOD>>().Each([&count](const Components::BoneLOD& restored)
    {
        ++count;
        EXPECT_FLOAT_EQ(restored.SwitchCoverage, 0.125f);
        EXPECT_TRUE(restored.FullDetailOverride);
    });
    EXPECT_EQ(count, 1u);
}
