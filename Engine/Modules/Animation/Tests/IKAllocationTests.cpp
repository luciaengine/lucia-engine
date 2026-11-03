#include <gtest/gtest.h>

#include "Animation/AllocationGuard.h"
#include "Animation/AnimGraphNode.h"
#include "Animation/AnimationPose.h"
#include "Animation/EvaluationContext.h"
#include "Animation/PoseUtilities.h"
#include "Animation/Nodes/FABRIKNode.h"
#include "Animation/Nodes/FootIKNode.h"
#include "Animation/Nodes/LookAtNode.h"
#include "Animation/Nodes/TwoBoneIKNode.h"

#include <Mathematics/Quaternion.h>
#include <Mathematics/Vector3.h>

#include <memory>
#include <vector>

using namespace GameEngine::Animation;
using GameEngine::Mathematics::Quaternion;
using GameEngine::Mathematics::Vector3;

namespace
{

// Echo a stored pose; deliberately does not allocate per Evaluate.
class StaticPoseNode : public AnimGraphNode
{
public:
    AnimationPose Pose;
    void Evaluate(EvaluationContext& /*ctx*/, AnimationPose& outPose) override
    {
        outPose = Pose;
    }
};

std::unique_ptr<StaticPoseNode> MakeStatic(const AnimationPose& pose)
{
    auto node = std::make_unique<StaticPoseNode>();
    node->Pose = pose;
    return node;
}

EvaluationContext MakeCtx(float dt)
{
    EvaluationContext ctx;
    ctx.DeltaTime = dt;
    return ctx;
}

void BuildStraightChain3(AnimationPose& pose, std::vector<int32_t>& parents)
{
    pose.Resize(3);
    pose.Positions[0] = Vector3(0.0f, 0.0f, 0.0f);
    pose.Positions[1] = Vector3(1.0f, 0.0f, 0.0f);
    pose.Positions[2] = Vector3(1.0f, 0.0f, 0.0f);
    parents = {-1, 0, 1};
}

void BuildStraightChain4(AnimationPose& pose, std::vector<int32_t>& parents)
{
    pose.Resize(4);
    pose.Positions[0] = Vector3(0.0f, 0.0f, 0.0f);
    pose.Positions[1] = Vector3(1.0f, 0.0f, 0.0f);
    pose.Positions[2] = Vector3(1.0f, 0.0f, 0.0f);
    pose.Positions[3] = Vector3(1.0f, 0.0f, 0.0f);
    parents = {-1, 0, 1, 2};
}

} // namespace

// AllocationGuard hook lives in Engine/Source/Memory/MimallocOverrides.cpp
// under GE_DEV_DIAG. Debug + DebugFast + RelWithDebInfo wire it; Release
// trivially passes. The HookCatchesAllocation test below verifies the
// hook is active by intentionally allocating inside a guarded scope.

#if defined(GE_DEV_DIAG)
TEST(IKAllocationTests, HookCatchesAllocation)
{
    AllocationGuard guard;
    // Force a heap allocation that the hook MUST observe.
    auto* p = new int[64];
    std::size_t observed = guard.Count();
    delete[] p;
    EXPECT_GE(observed, 1u)
        << "AllocationGuard hook is not wired; the IK '0 allocations' tests"
           " would be vacuous. See MimallocOverrides.cpp.";
}
#endif

TEST(IKAllocationTests, TwoBoneIK_NoAllocsAfterWarmup)
{
    AnimationPose pose;
    std::vector<int32_t> parents;
    BuildStraightChain3(pose, parents);

    TwoBoneIKNode ik;
    ik.SetSource(MakeStatic(pose));
    ik.SetBoneIndices(0, 1, 2);
    ik.SetParentIndices(parents);
    ik.SetTarget(Vector3(1.0f, 1.0f, 0.0f));
    ik.SetPoleTarget(Vector3(0.0f, 0.0f, 1.0f));
    ik.SetWeight(1.0f);

    AnimationPose result;
    result.Resize(3);

    // Warm up: lets m_ScratchWorld / output pose vectors size themselves.
    auto ctx = MakeCtx(0.016f);
    for (int i = 0; i < 4; ++i)
        ik.Evaluate(ctx, result);

    AllocationGuard guard;
    for (int i = 0; i < 32; ++i)
        ik.Evaluate(ctx, result);
    EXPECT_EQ(guard.Count(), 0u);
}

TEST(IKAllocationTests, FABRIK_NoAllocsAfterWarmup)
{
    AnimationPose pose;
    std::vector<int32_t> parents;
    BuildStraightChain4(pose, parents);

    FABRIKNode fabrik;
    fabrik.SetSource(MakeStatic(pose));
    fabrik.SetChain({0, 1, 2, 3});
    fabrik.SetParentIndices(parents);
    fabrik.SetTarget(Vector3(2.0f, 1.0f, 0.0f));
    fabrik.SetWeight(1.0f);
    fabrik.SetMaxIterations(10);

    AnimationPose result;
    result.Resize(4);

    auto ctx = MakeCtx(0.016f);
    for (int i = 0; i < 4; ++i)
        fabrik.Evaluate(ctx, result);

    AllocationGuard guard;
    for (int i = 0; i < 32; ++i)
        fabrik.Evaluate(ctx, result);
    EXPECT_EQ(guard.Count(), 0u);
}

TEST(IKAllocationTests, LookAt_NoAllocsAfterWarmup)
{
    AnimationPose pose;
    pose.Resize(2);
    pose.Positions[0] = Vector3(0.0f, 0.0f, 0.0f);
    pose.Positions[1] = Vector3(0.0f, 1.0f, 0.0f);
    std::vector<int32_t> parents = {-1, 0};

    LookAtNode lookAt;
    lookAt.SetSource(MakeStatic(pose));
    lookAt.SetBoneIndex(1);
    lookAt.SetParentIndices(parents);
    lookAt.SetTarget(Vector3(0.0f, 1.0f, 5.0f));
    lookAt.SetAimAxis(Vector3(0.0f, 0.0f, 1.0f));
    lookAt.SetWeight(1.0f);
    lookAt.SetMaxAngle(3.14159f);

    AnimationPose result;
    result.Resize(2);

    auto ctx = MakeCtx(0.016f);
    for (int i = 0; i < 4; ++i)
        lookAt.Evaluate(ctx, result);

    AllocationGuard guard;
    for (int i = 0; i < 32; ++i)
        lookAt.Evaluate(ctx, result);
    EXPECT_EQ(guard.Count(), 0u);
}

TEST(IKAllocationTests, FootIK_NoAllocsAfterWarmup)
{
    AnimationPose pose;
    pose.Resize(4);
    pose.Positions[0] = Vector3(0.0f, 2.0f, 0.0f);
    pose.Positions[1] = Vector3(0.0f, 0.0f, 0.0f);
    pose.Positions[2] = Vector3(0.0f, -1.0f, 0.0f);
    pose.Positions[3] = Vector3(0.0f, -1.0f, 0.0f);
    std::vector<int32_t> parents = {-1, 0, 1, 2};

    FootIKNode footIK;
    footIK.SetSource(MakeStatic(pose));
    footIK.SetParentIndices(parents);
    footIK.SetHipBoneIndex(1);
    footIK.SetMaxHipOffset(1.0f);
    footIK.SetWeight(1.0f);

    FootIKLeg leg;
    leg.HipBone = 1;
    leg.KneeBone = 2;
    leg.FootBone = 3;
    footIK.AddLeg(leg);

    FootIKGroundResult ground;
    ground.Hit = true;
    ground.Position = Vector3(0.0f, -0.3f, 0.0f);
    ground.Normal = Vector3(0.0f, 1.0f, 0.0f);
    footIK.SetGroundResult(0, ground);

    AnimationPose result;
    result.Resize(4);

    auto ctx = MakeCtx(0.016f);
    for (int i = 0; i < 4; ++i)
        footIK.Evaluate(ctx, result);

    AllocationGuard guard;
    for (int i = 0; i < 32; ++i)
        footIK.Evaluate(ctx, result);
    EXPECT_EQ(guard.Count(), 0u);
}
