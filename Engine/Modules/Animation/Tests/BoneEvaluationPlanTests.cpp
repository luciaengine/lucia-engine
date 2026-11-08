#include "Animation/BoneEvaluationPlan.h"
#include "Animation/SkeletonData.h"
#include "Animation/PoseToSkinMatrices.h"
#include "Mathematics/Vector3.h"

#include <algorithm>
#include <limits>

#include <gtest/gtest.h>

using namespace GameEngine;
using namespace GameEngine::Animation;
using namespace GameEngine::Mathematics;

namespace
{

SkeletonData MakePlanSkeleton(std::vector<int32> parents)
{
    SkeletonData skeleton;
    skeleton.BoneCount = static_cast<uint32>(parents.size());
    skeleton.Parent = std::move(parents);
    skeleton.RestLocalMatrix.resize(static_cast<size_t>(skeleton.BoneCount) * 16u);
    skeleton.InverseBind.resize(static_cast<size_t>(skeleton.BoneCount) * 16u);
    const Matrix4x4 identity;
    for (uint32 node = 0; node < skeleton.BoneCount; ++node)
    {
        const Matrix4x4 local = Matrix4x4::Translation(Vector3(static_cast<float>(node + 1u), 0.0f, 0.0f));
        std::copy_n(local.Data(), 16u, skeleton.RestLocalMatrix.data() + static_cast<size_t>(node) * 16u);
        std::copy_n(identity.Data(), 16u, skeleton.InverseBind.data() + static_cast<size_t>(node) * 16u);
    }
    skeleton.ComputeTopologicalSort();
    return skeleton;
}

std::vector<uint32> CopyNodes(const BoneEvaluationPlan& plan)
{
    return {plan.Nodes().begin(), plan.Nodes().end()};
}

PoseSampleWorkspace MakePlanWorkspace(const SkeletonData& skeleton)
{
    PoseSampleWorkspace workspace;
    workspace.Pose.resize(skeleton.BoneCount);
    workspace.ChangedTranslation.assign(skeleton.BoneCount, 0u);
    workspace.ChangedRotation.assign(skeleton.BoneCount, 0u);
    workspace.ChangedScale.assign(skeleton.BoneCount, 0u);
    return workspace;
}

void ExpectMatricesNear(std::span<const float> actual, std::span<const float> expected)
{
    ASSERT_EQ(actual.size(), expected.size());
    for (size_t entry = 0u; entry < actual.size(); ++entry)
        EXPECT_NEAR(actual[entry], expected[entry], 0.00001f) << "entry " << entry;
}

} // namespace

TEST(BoneEvaluationPlan, RetainsRequestedLeafAncestorsAndAllRootsInStableLevels)
{
    const SkeletonData skeleton = MakePlanSkeleton({2, -1, 1, 2, -1, 3});
    const uint32 requested[] = {5u, 5u};
    BoneEvaluationPlan plan;
    ASSERT_TRUE(plan.Build(skeleton, requested));
    EXPECT_EQ(CopyNodes(plan), (std::vector<uint32>{1u, 4u, 2u, 3u, 5u}));
    EXPECT_EQ((std::vector<uint32>{plan.SkinLevelOffsets().begin(), plan.SkinLevelOffsets().end()}),
        (std::vector<uint32>{0u, 2u, 3u, 4u, 5u}));
    EXPECT_EQ(plan.EvaluatedAncestors()[0], 2u);
    EXPECT_EQ(plan.EvaluatedAncestors()[5], 5u);
}

TEST(BoneEvaluationPlan, ReconstructsOmittedChainWithoutInvertingSingularRestTransforms)
{
    SkeletonData skeleton = MakePlanSkeleton({-1, 0, 1, 2});
    Matrix4x4 local1;
    local1[0][0] = 0.0f;
    local1[1][0] = 0.5f;
    local1[1][1] = 2.0f;
    local1[3][0] = 7.0f;
    std::copy_n(local1.Data(), 16u, skeleton.RestLocalMatrix.data() + 16u);
    BoneEvaluationPlan plan;
    ASSERT_TRUE(plan.Build(skeleton, {}));
    ASSERT_EQ(plan.Nodes().size(), 1u);
    const Matrix4x4 animatedRoot = Matrix4x4::Translation(Vector3(100.0f, 5.0f, 2.0f));
    Matrix4x4 expected = animatedRoot;
    for (uint32 node = 1u; node < skeleton.BoneCount; ++node)
    {
        expected = expected * Matrix4x4::FromColumnMajor(skeleton.RestLocalMatrix.data() + node * 16u);
        const Matrix4x4 actual = animatedRoot * plan.RestFromAncestors()[node];
        EXPECT_EQ(plan.EvaluatedAncestors()[node], 0u);
        for (uint32 entry = 0u; entry < 16u; ++entry)
            EXPECT_FLOAT_EQ(actual.Data()[entry], expected.Data()[entry]);
    }
}

TEST(BoneEvaluationPlan, FullSelectionKeepsOriginalNodeIndicesAndIdentityCorrections)
{
    const SkeletonData skeleton = MakePlanSkeleton({2, -1, 1, 2});
    BoneEvaluationPlan plan;
    ASSERT_TRUE(plan.Build(skeleton, skeleton.TopologicalOrder));
    EXPECT_EQ(CopyNodes(plan), skeleton.TopologicalOrder);
    const Matrix4x4 identity;
    for (uint32 node = 0; node < skeleton.BoneCount; ++node)
    {
        EXPECT_EQ(plan.EvaluatedAncestors()[node], node);
        EXPECT_TRUE(std::equal(identity.Data(), identity.Data() + 16u, plan.RestFromAncestors()[node].Data()));
    }
}

TEST(BoneEvaluationPlan, PaletteEvaluationExcludesUnrelatedRootsButRetainsRequestedNodeWorlds)
{
    SkeletonData skeleton = MakePlanSkeleton({-1, 0, 1, 2, -1, 4});
    skeleton.SkinJointCount = 1u;
    skeleton.JointNodes = {3u};
    skeleton.ComputeTopologicalSort();
    const uint32 requested[] = {1u, 5u};
    BoneEvaluationPlan plan;
    ASSERT_TRUE(plan.Build(skeleton, requested));
    EXPECT_EQ(CopyNodes(plan), (std::vector<uint32>{0u, 4u, 1u, 5u}));
    EXPECT_EQ((std::vector<uint32>{plan.SkinNodes().begin(), plan.SkinNodes().end()}),
        (std::vector<uint32>{0u, 1u}));
    EXPECT_EQ((std::vector<uint32>{plan.SkinLevelOffsets().begin(), plan.SkinLevelOffsets().end()}),
        (std::vector<uint32>{0u, 1u, 2u}));
}

TEST(BoneEvaluationPlan, RejectsInvalidSelectionAndClearsPreviousPlan)
{
    const SkeletonData skeleton = MakePlanSkeleton({-1, 0});
    BoneEvaluationPlan plan;
    ASSERT_TRUE(plan.Build(skeleton, {}));
    const uint32 invalid[] = {2u};
    EXPECT_FALSE(plan.Build(skeleton, invalid));
    EXPECT_TRUE(plan.Nodes().empty());
    EXPECT_TRUE(plan.SkinLevelOffsets().empty());
    EXPECT_TRUE(plan.EvaluatedAncestors().empty());
    EXPECT_TRUE(plan.RestFromAncestors().empty());
    EXPECT_TRUE(plan.JointCorrections().empty());
}

TEST(BoneEvaluationPlan, RejectsCyclesOutOfRangeParentsAndStaleTopology)
{
    BoneEvaluationPlan plan;
    for (const auto& parents : {std::vector<int32>{1, 0}, std::vector<int32>{-1, 8}})
    {
        const SkeletonData skeleton = MakePlanSkeleton(parents);
        EXPECT_FALSE(plan.Build(skeleton, {}));
    }
    SkeletonData skeleton = MakePlanSkeleton({-1, 0, 1});
    skeleton.TopologicalOrder = {0u, 2u, 1u};
    EXPECT_FALSE(plan.Build(skeleton, {}));
    skeleton.TopologicalOrder = {0u, 1u, 1u};
    EXPECT_FALSE(plan.Build(skeleton, {}));
}

TEST(BoneEvaluationPlan, RejectsMissingAndNonFiniteRestData)
{
    BoneEvaluationPlan plan;
    SkeletonData skeleton = MakePlanSkeleton({-1, 0});
    skeleton.RestLocalMatrix[17] = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(plan.Build(skeleton, {}));
    skeleton.RestLocalMatrix.clear();
    EXPECT_FALSE(plan.Build(skeleton, {}));
}

TEST(BoneEvaluationPlan, RejectsOverflowInPrecomputedRestChainAndClearsPartialOutput)
{
    SkeletonData skeleton = MakePlanSkeleton({-1, 0, 1});
    skeleton.RestLocalMatrix[16u] = 1e30f;
    skeleton.RestLocalMatrix[32u] = 1e30f;
    BoneEvaluationPlan plan;
    EXPECT_FALSE(plan.Build(skeleton, {}));
    EXPECT_TRUE(plan.Nodes().empty());
    EXPECT_TRUE(plan.RestFromAncestors().empty());
    EXPECT_TRUE(plan.JointCorrections().empty());
}

TEST(BoneEvaluationPlan, EmptySkeletonHasNoLevelsOrNodes)
{
    const SkeletonData skeleton;
    BoneEvaluationPlan plan;
    ASSERT_TRUE(plan.Build(skeleton, {}));
    EXPECT_TRUE(plan.Nodes().empty());
    EXPECT_EQ(plan.SkinLevelOffsets().size(), 1u);
    EXPECT_EQ(plan.SkinLevelOffsets()[0], 0u);
}

TEST(BoneEvaluationPlan, ReducedSkinPaletteMatchesRestClampedReferenceWithMeshRootAndJointPermutation)
{
    SkeletonData skeleton = MakePlanSkeleton({-1, 0, 1, 2, 1, 4});
    Matrix4x4 shearedRest = Matrix4x4::FromColumnMajor(skeleton.RestLocalMatrix.data() + 64u);
    shearedRest[1][0] = 0.25f;
    shearedRest[1][1] = 2.0f;
    std::copy_n(shearedRest.Data(), 16u, skeleton.RestLocalMatrix.data() + 64u);
    const Matrix4x4 meshRoot = Matrix4x4::Translation(Vector3(10.0f, -3.0f, 2.0f));
    std::copy_n(meshRoot.Data(), 16u, skeleton.MeshRootWorld);
    skeleton.InverseBind.resize(skeleton.BoneCount * 16u);
    for (uint32 node = 0; node < skeleton.BoneCount; ++node)
    {
        const Matrix4x4 inverseBind = Matrix4x4::Translation(Vector3(-static_cast<float>(node), 2.0f, 1.0f));
        std::copy_n(inverseBind.Data(), 16u, skeleton.InverseBind.data() + node * 16u);
    }
    skeleton.SkinJointCount = 4u;
    skeleton.JointNodes = {5u, 3u, 0u, 2u};
    skeleton.ComputeTopologicalSort();
    const uint32 requested[] = {2u};
    BoneEvaluationPlan plan;
    ASSERT_TRUE(plan.Build(skeleton, requested));
    PoseSampleWorkspace reference = MakePlanWorkspace(skeleton);
    for (uint32 node : plan.Nodes())
    {
        reference.Pose[node].t = glm::vec3(0.3f * static_cast<float>(node), 1.0f, -2.0f);
        reference.Pose[node].r = glm::angleAxis(0.5f, glm::vec3(0.0f, 0.0f, 1.0f));
        reference.Pose[node].s = glm::vec3(0.5f, 2.0f, 1.0f);
        reference.ChangedTranslation[node] = 1u;
    }
    PoseSampleWorkspace reduced = reference;
    // Omitted-node animation and stale output must not leak into the reduced palette.
    for (uint32 node : {3u, 4u, 5u})
    {
        reduced.Pose[node].t = glm::vec3(9000.0f);
        reduced.ChangedTranslation[node] = 1u;
    }
    std::vector<float> expectedPalette;
    std::vector<float> actualPalette(1000u, 999.0f);
    std::vector<float> expectedWorld;
    std::vector<float> actualWorld;
    BuildPoseToSkinMatrices(skeleton, &expectedWorld, &expectedPalette, reference);
    BuildPoseToSkinMatrices(skeleton, &actualWorld, &actualPalette, reduced, &plan);
    ASSERT_EQ(actualPalette.size(), 4u * 16u);
    ExpectMatricesNear(actualPalette, expectedPalette);
    ExpectMatricesNear(actualWorld, expectedWorld);
    actualPalette.assign(1000u, 999.0f);
    BuildPoseToSkinMatrices(skeleton, nullptr, &actualPalette, reduced, &plan);
    ExpectMatricesNear(actualPalette, expectedPalette);
}

TEST(BoneEvaluationPlan, FullPlanMatchesOrdinaryEvaluationExactlyWithImplicitJointTable)
{
    SkeletonData skeleton = MakePlanSkeleton({2, -1, 1, 2});
    const Matrix4x4 identity;
    skeleton.InverseBind.resize(skeleton.BoneCount * 16u);
    for (uint32 node = 0; node < skeleton.BoneCount; ++node)
        std::copy_n(identity.Data(), 16u, skeleton.InverseBind.data() + node * 16u);
    BoneEvaluationPlan plan;
    ASSERT_TRUE(plan.Build(skeleton, skeleton.TopologicalOrder));
    PoseSampleWorkspace ordinary = MakePlanWorkspace(skeleton);
    PoseSampleWorkspace planned = ordinary;
    std::vector<float> ordinaryWorld;
    std::vector<float> plannedWorld;
    std::vector<float> ordinaryPalette;
    std::vector<float> plannedPalette;
    BuildPoseToSkinMatrices(skeleton, &ordinaryWorld, &ordinaryPalette, ordinary);
    BuildPoseToSkinMatrices(skeleton, &plannedWorld, &plannedPalette, planned, &plan);
    EXPECT_EQ(plannedWorld, ordinaryWorld);
    EXPECT_EQ(plannedPalette, ordinaryPalette);
    EXPECT_EQ(plannedPalette.size(), skeleton.BoneCount * 16u);
}

TEST(BoneEvaluationPlan, AnotherSkeletonCannotUseThePlanEvenWithTheSameNodeCount)
{
    const SkeletonData first = MakePlanSkeleton({-1, 0});
    const SkeletonData second = MakePlanSkeleton({-1, 0});
    BoneEvaluationPlan plan;
    ASSERT_TRUE(plan.Build(first, {}));
    EXPECT_TRUE(plan.IsForSkeleton(first));
    EXPECT_FALSE(plan.IsForSkeleton(second));
}

TEST(BoneEvaluationPlan, CrossfadeRejectsForeignPlanAndPreservesOrdinaryBlend)
{
    const SkeletonData skeleton = MakePlanSkeleton({-1, 0, 1});
    const SkeletonData foreign = MakePlanSkeleton({-1, 0, 1});
    BoneEvaluationPlan plan;
    ASSERT_TRUE(plan.Build(foreign, {}));
    PoseSampleWorkspace current = MakePlanWorkspace(skeleton);
    PoseSampleWorkspace previous = current;
    for (uint32 node = 0; node < skeleton.BoneCount; ++node)
    {
        current.Pose[node].t = glm::vec3(10.0f);
        previous.Pose[node].t = glm::vec3(2.0f);
    }
    BlendLocalPoses(skeleton, current, previous, 0.25f, &plan);
    for (uint32 node = 0; node < skeleton.BoneCount; ++node)
        EXPECT_FLOAT_EQ(current.Pose[node].t.x, 4.0f);
}

TEST(BoneEvaluationPlan, CrossfadeBlendsRetainedNodesAndLeavesOmittedLocalsUnsampled)
{
    const SkeletonData skeleton = MakePlanSkeleton({-1, 0, 1});
    BoneEvaluationPlan plan;
    ASSERT_TRUE(plan.Build(skeleton, {}));
    PoseSampleWorkspace current = MakePlanWorkspace(skeleton);
    PoseSampleWorkspace previous = current;
    for (uint32 node = 0; node < skeleton.BoneCount; ++node)
    {
        current.Pose[node].t = glm::vec3(10.0f);
        previous.Pose[node].t = glm::vec3(2.0f);
    }
    BlendLocalPoses(skeleton, current, previous, 0.25f, &plan);
    EXPECT_FLOAT_EQ(current.Pose[0].t.x, 4.0f);
    EXPECT_FLOAT_EQ(current.Pose[1].t.x, 10.0f);
    EXPECT_FLOAT_EQ(current.Pose[2].t.x, 10.0f);
    EXPECT_EQ(current.ChangedTranslation, (std::vector<uint8>{1u, 0u, 0u}));
}
