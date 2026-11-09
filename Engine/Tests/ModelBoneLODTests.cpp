#include "Assets/BoneLOD/ModelBoneLOD.h"
#include "Animation/BoneEvaluationPlan.h"
#include "Animation/SkeletonData.h"
#include "Assets/BoneLOD/BoneLODAssetSettings.h"
#include "Assets/ModelAsset.h"
#include "Assets/AssetManager.h"
#include "Assets/ModelAssetSettings.h"
#include "ECSModules/Rendering/SkeletonStore.h"
#include "Mathematics/Matrix4x4.h"
#include "Mathematics/MatrixOps.h"
#include "TestTempDir.h"

#include <algorithm>
#include <fstream>
#include <limits>

#include <gtest/gtest.h>

using namespace GameEngine;

namespace
{

Animation::SkeletonData MakeReductionSkeleton(std::vector<int32> parents)
{
    Animation::SkeletonData skeleton;
    skeleton.BoneCount = static_cast<uint32>(parents.size());
    skeleton.Parent = std::move(parents);
    const Mathematics::Matrix4x4 identity;
    skeleton.RestLocalMatrix.resize(skeleton.BoneCount * 16u);
    skeleton.InverseBind.resize(skeleton.BoneCount * 16u);
    for (uint32 node = 0; node < skeleton.BoneCount; ++node)
    {
        std::copy_n(identity.Data(), 16u, skeleton.RestLocalMatrix.data() + node * 16u);
        std::copy_n(identity.Data(), 16u, skeleton.InverseBind.data() + node * 16u);
    }
    skeleton.ComputeTopologicalSort();
    return skeleton;
}

Mesh MakeInfluencedMesh(uint16 joint, float distance, uint32 vertexCount = 1u)
{
    Mesh mesh;
    mesh.Skinned = true;
    mesh.Vertices.resize(vertexCount);
    mesh.Joints0.assign(vertexCount * 4u, 0u);
    mesh.Weights0.assign(vertexCount * 4u, 0.0f);
    for (uint32 vertex = 0; vertex < vertexCount; ++vertex)
    {
        mesh.Vertices[vertex].Position[0] = distance;
        mesh.Joints0[vertex * 4u] = joint;
        mesh.Weights0[vertex * 4u] = 1.0f;
    }
    return mesh;
}

bool Retains(const ModelBoneLOD& data, uint32 node)
{
    return data.ReducedPlan() && data.ReducedPlan()->EvaluatedAncestors()[node] == node;
}

} // namespace

TEST(ModelBoneLOD, ScoresAllMeshesWithoutFavoringRepeatedLowImpactVertices)
{
    const auto skeleton = MakeReductionSkeleton({-1, 0, 0, 0});
    ModelAsset model(GUID::Generate(), "Synthetic://BoneLOD");
    model.SetMeshesForTest({MakeInfluencedMesh(1u, 2.0f, 1000u), MakeInfluencedMesh(2u, 3.0f)});
    BoneLODAssetSettings settings;
    settings.BoneBudget = 2u;
    ModelBoneLOD data;
    ASSERT_TRUE(data.Build(model, skeleton, settings));
    EXPECT_EQ(data.ActualBoneCount(), 2u);
    EXPECT_TRUE(Retains(data, 0u));
    EXPECT_TRUE(Retains(data, 2u));
    EXPECT_FALSE(Retains(data, 1u));
}

TEST(ModelBoneLOD, AccumulatesDuplicateJointWeightsAcrossBothInfluenceSets)
{
    const auto skeleton = MakeReductionSkeleton({-1, 0, 0, 0});
    Mesh mesh = MakeInfluencedMesh(1u, 1.0f);
    mesh.Weights0[0] = 0.3f;
    mesh.Joints0[1] = 2u;
    mesh.Weights0[1] = 0.4f;
    mesh.Joints1 = {1u, 0u, 0u, 0u};
    mesh.Weights1 = {0.3f, 0.0f, 0.0f, 0.0f};
    ModelAsset model(GUID::Generate(), "Synthetic://BoneLOD");
    model.SetMeshesForTest({mesh});
    BoneLODAssetSettings settings;
    settings.BoneBudget = 2u;
    ModelBoneLOD data;
    ASSERT_TRUE(data.Build(model, skeleton, settings));
    EXPECT_TRUE(Retains(data, 1u));
    EXPECT_FALSE(Retains(data, 2u));
}

TEST(ModelBoneLOD, RankingUsesOriginalJointMappingRestSpaceAndMeshRoot)
{
    auto skeleton = MakeReductionSkeleton({-1, 0, 0, 0});
    skeleton.SkinJointCount = 2u;
    skeleton.JointNodes = {2u, 1u};
    auto firstRest = Mathematics::Matrix4x4::Translation(Mathematics::Vector3(5.0f, 0.0f, 0.0f));
    firstRest[0][0] = 2.0f;
    auto secondRest = Mathematics::Matrix4x4::Translation(Mathematics::Vector3(10.0f, 0.0f, 0.0f));
    secondRest[0][0] = 10.0f;
    const auto firstInverse = Mathematics::Inverse(firstRest);
    const auto secondInverse = Mathematics::Inverse(secondRest);
    std::copy_n(firstRest.Data(), 16u, skeleton.RestLocalMatrix.data() + 16u);
    std::copy_n(secondRest.Data(), 16u, skeleton.RestLocalMatrix.data() + 32u);
    std::copy_n(firstInverse.Data(), 16u, skeleton.InverseBind.data() + 16u);
    std::copy_n(secondInverse.Data(), 16u, skeleton.InverseBind.data() + 32u);
    skeleton.MeshRootWorld[0] = 2.0f;
    skeleton.MeshRootWorld[5] = 0.5f;
    skeleton.ComputeTopologicalSort();
    BoneLODAssetSettings settings;
    settings.BoneBudget = 2u;
    ModelAsset model(GUID::Generate(), "Synthetic://BoneLOD");
    ModelBoneLOD data;
    model.SetMeshesForTest({MakeInfluencedMesh(0u, 10.1f), MakeInfluencedMesh(1u, 5.2f)});
    ASSERT_TRUE(data.Build(model, skeleton, settings));
    EXPECT_TRUE(Retains(data, 1u));
    EXPECT_FALSE(Retains(data, 2u));
    model.SetMeshesForTest({MakeInfluencedMesh(0u, 11.5f), MakeInfluencedMesh(1u, 6.0f)});
    ASSERT_TRUE(data.Build(model, skeleton, settings));
    EXPECT_TRUE(Retains(data, 2u));
    Mesh second = MakeInfluencedMesh(0u, 10.0f);
    second.Vertices[0].Position[1] = 1.0f;
    model.SetMeshesForTest({second, MakeInfluencedMesh(1u, 6.1f)});
    ASSERT_TRUE(data.Build(model, skeleton, settings));
    EXPECT_TRUE(Retains(data, 2u));
}

TEST(ModelBoneLOD, MorphSpatialExtentContributesToTheQualityHeuristic)
{
    const auto skeleton = MakeReductionSkeleton({-1, 0, 0, 0});
    Mesh first = MakeInfluencedMesh(1u, 0.1f);
    MorphTarget target;
    target.VertexIndices = {0u};
    target.PositionDeltas = {10.0f, 0.0f, 0.0f};
    first.MorphTargets.push_back(target);
    ModelAsset model(GUID::Generate(), "Synthetic://BoneLOD");
    model.SetMeshesForTest({first, MakeInfluencedMesh(2u, 1.0f)});
    BoneLODAssetSettings settings;
    settings.BoneBudget = 2u;
    ModelBoneLOD data;
    ASSERT_TRUE(data.Build(model, skeleton, settings));
    EXPECT_TRUE(Retains(data, 1u));
}

TEST(ModelBoneLOD, RequiredPathsRetainEveryAmbiguousMatchAndOverrideBudget)
{
    auto skeleton = MakeReductionSkeleton({-1, 0, 1, 0, 3, 0});
    skeleton.BoneNames = {"Root", "Arm", "Hand", "Arm", "Hand", "Other"};
    ModelAsset model(GUID::Generate(), "Synthetic://BoneLOD");
    model.SetMeshesForTest({MakeInfluencedMesh(5u, 100.0f)});
    BoneLODAssetSettings settings;
    settings.BoneBudget = 2u;
    settings.MandatoryPaths = {{"Root", "Arm", "Hand"}};
    ModelBoneLOD data;
    ASSERT_TRUE(data.Build(model, skeleton, settings));
    EXPECT_EQ(data.RequestedBoneBudget(), 2u);
    EXPECT_EQ(data.RequiredBoneCount(), 5u);
    EXPECT_EQ(data.ActualBoneCount(), 5u);
    EXPECT_FALSE(data.Warning().empty());
    EXPECT_TRUE(Retains(data, 2u));
    EXPECT_TRUE(Retains(data, 4u));
    EXPECT_FALSE(Retains(data, 5u));
}

TEST(ModelBoneLOD, UnrelatedRootsDoNotConsumeTheSkinEvaluationBudget)
{
    auto skeleton = MakeReductionSkeleton({-1, 0, 1, 0, 3, -1, -1, -1});
    skeleton.SkinJointCount = 4u;
    skeleton.JointNodes = {1u, 2u, 3u, 4u};
    skeleton.ComputeTopologicalSort();
    ModelAsset model(GUID::Generate(), "Synthetic://BoneLOD");
    model.SetMeshesForTest({MakeInfluencedMesh(1u, 10.0f)});
    BoneLODAssetSettings settings;
    settings.BoneBudget = 3u;
    ModelBoneLOD data;
    ASSERT_TRUE(data.Build(model, skeleton, settings));
    EXPECT_EQ(data.RequiredBoneCount(), 1u);
    EXPECT_EQ(data.ActualBoneCount(), 3u);
    ASSERT_TRUE(data.ReducedPlan());
    EXPECT_EQ(data.ReducedPlan()->Nodes().size(), 6u);
    EXPECT_EQ(data.ReducedPlan()->SkinNodes().size(), 3u);
    EXPECT_TRUE(Retains(data, 1u));
    EXPECT_TRUE(Retains(data, 2u));
}

TEST(ModelBoneLOD, MissingMandatoryPathDisablesReductionInsteadOfDroppingRequirement)
{
    auto skeleton = MakeReductionSkeleton({-1, 0, 0});
    skeleton.BoneNames = {"Root", "Hand", "Foot"};
    ModelAsset model(GUID::Generate(), "Synthetic://BoneLOD");
    model.SetMeshesForTest({MakeInfluencedMesh(1u, 1.0f)});
    BoneLODAssetSettings settings;
    settings.BoneBudget = 2u;
    settings.MandatoryPaths = {{"Root", "Missing"}};
    ModelBoneLOD data;
    EXPECT_FALSE(data.Build(model, skeleton, settings));
    EXPECT_FALSE(data.ReducedPlan());
    EXPECT_FALSE(data.Error().empty());
    EXPECT_EQ(data.ActualBoneCount(), skeleton.BoneCount);
}

TEST(ModelBoneLOD, RebindCreatesIndependentPlanForCopiedPayload)
{
    const auto original = MakeReductionSkeleton({-1, 0, 0});
    auto live = original;
    live.RestLocalMatrix[2u * 16u + 12u] = 7.0f;
    ModelAsset model(GUID::Generate(), "Synthetic://BoneLOD");
    model.SetMeshesForTest({MakeInfluencedMesh(1u, 1.0f)});
    BoneLODAssetSettings settings;
    settings.BoneBudget = 2u;
    ModelBoneLOD data;
    ASSERT_TRUE(data.Build(model, original, settings));
    const auto rebound = data.Rebind(live);
    ASSERT_TRUE(rebound->ReducedPlan());
    EXPECT_NE(rebound->ReducedPlan(), data.ReducedPlan());
    EXPECT_TRUE(rebound->ReducedPlan()->IsForSkeleton(live));
    EXPECT_TRUE(data.ReducedPlan()->IsForSkeleton(original));
    EXPECT_FLOAT_EQ(rebound->ReducedPlan()->RestFromAncestors()[2u].Data()[12u], 7.0f);
}

TEST(ModelBoneLOD, InvalidPositiveInfluenceOrNonFiniteGeometryKeepsFullDetail)
{
    const auto skeleton = MakeReductionSkeleton({-1, 0, 0});
    BoneLODAssetSettings settings;
    settings.BoneBudget = 2u;
    for (const Mesh& mesh : {MakeInfluencedMesh(9u, 1.0f),
        MakeInfluencedMesh(1u, std::numeric_limits<float>::quiet_NaN())})
    {
        ModelAsset model(GUID::Generate(), "Synthetic://BoneLOD");
        model.SetMeshesForTest({mesh});
        ModelBoneLOD data;
        EXPECT_FALSE(data.Build(model, skeleton, settings));
        EXPECT_FALSE(data.ReducedPlan());
        EXPECT_FALSE(data.Error().empty());
    }
}

TEST(ModelBoneLOD, StagedAdoptionPreservesLiveSkeletonAndRebindsEditedMandatoryRequirements)
{
    TestUtils::ScopedTempDir temporary{TestUtils::MakeUniqueTempDirectory("bone_lod_reload")};
    const auto root = temporary.Path() / "Assets";
    const auto path = root / "Character.fbx";
    std::filesystem::create_directories(root);
    std::ofstream(path) << "metadata fixture";
    AssetManager assets;
    ASSERT_TRUE(assets.Initialize(root, nullptr, temporary.Path() / "AssetDatabase.assetdb",
        temporary.Path() / ".Cache"));
    assets.WaitForStartupScan();
    ModelAssetSettings rigSettings;
    rigSettings.RigKind = ModelRigKind::NotHumanoid;
    ASSERT_TRUE(rigSettings.Save(assets.GetRegistry(), path));
    AssetManager::ScopedThreadAssetManager loading(&assets);
    auto original = MakeReductionSkeleton({-1, 0, 0, 0, 0});
    original.BoneNames = {"Root", "Bone1", "Bone2", "Bone3", "Bone4"};
    auto stagedData = original;
    stagedData.RestLocalMatrix[4u * 16u + 12u] = 7.0f;
    auto& skeletons = Engine::Renderer::SkeletonStore::Instance();
    const uint32 liveId = skeletons.CreateSkeleton(original.BoneCount);
    const uint32 stagedId = skeletons.CreateSkeleton(stagedData.BoneCount);
    *skeletons.Get(liveId) = original;
    *skeletons.Get(stagedId) = stagedData;
    ModelAsset live(GUID::Generate(), path);
    live.SetSkeletonIdForTest(liveId);
    live.SetMeshesForTest({MakeInfluencedMesh(1u, 1.0f)});
    BoneLODAssetSettings settings;
    settings.BoneBudget = 2u;
    ASSERT_TRUE(settings.Save(assets.GetRegistry(), path));
    live.PostLoad();
    ASSERT_TRUE(live.GetBoneLOD());
    ASSERT_TRUE(live.GetBoneLOD()->ReducedPlan());
    EXPECT_TRUE(Retains(*live.GetBoneLOD(), 1u));

    settings.MandatoryPaths = {{"Root", "Bone3"}};
    ASSERT_TRUE(settings.Save(assets.GetRegistry(), path));
    ModelAsset staged(GUID::Generate(), path);
    staged.SetSkeletonIdForTest(stagedId);
    staged.SetMeshesForTest({MakeInfluencedMesh(1u, 1.0f)});
    staged.PostLoad();
    ASSERT_TRUE(staged.GetBoneLOD());
    const auto stagedPlan = staged.GetBoneLOD()->ReducedPlan();
    ASSERT_TRUE(stagedPlan);
    ASSERT_TRUE(live.AdoptReload(staged));
    staged.Unload();
    ASSERT_TRUE(live.GetBoneLOD()->ReducedPlan());
    EXPECT_EQ(live.GetSkeletonId(), liveId);
    EXPECT_NE(live.GetBoneLOD()->ReducedPlan(), stagedPlan);
    EXPECT_TRUE(live.GetBoneLOD()->ReducedPlan()->IsForSkeleton(*skeletons.Get(liveId)));
    EXPECT_TRUE(Retains(*live.GetBoneLOD(), 3u));
    EXPECT_FALSE(Retains(*live.GetBoneLOD(), 1u));
    EXPECT_FLOAT_EQ(live.GetBoneLOD()->ReducedPlan()->RestFromAncestors()[4u].Data()[12u], 7.0f);

    ASSERT_TRUE(assets.GetRegistry().SetMetaValue(path, BoneLODAssetSettings::kMetadataKey, "{"));
    ModelAsset malformed(GUID::Generate(), path);
    malformed.SetSkeletonIdForTest(stagedId);
    malformed.SetMeshesForTest({MakeInfluencedMesh(1u, 1.0f)});
    malformed.PostLoad();
    ASSERT_TRUE(live.AdoptReload(malformed));
    ASSERT_TRUE(live.GetBoneLOD());
    EXPECT_EQ(live.GetSkeletonId(), liveId);
    EXPECT_FALSE(live.GetBoneLOD()->ReducedPlan());
    EXPECT_FALSE(live.GetBoneLOD()->Error().empty());
    assets.Shutdown();
}
