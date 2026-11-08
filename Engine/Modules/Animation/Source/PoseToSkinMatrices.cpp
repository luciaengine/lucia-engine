#include "Animation/PoseToSkinMatrices.h"
#include "Animation/BoneEvaluationPlan.h"

#include <algorithm>
#include <cstring>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace GameEngine
{
namespace Animation
{

namespace
{

glm::mat4 BuildLocalTRS(const glm::vec3& t, const glm::quat& r, const glm::vec3& s)
{
    return glm::translate(glm::mat4(1.0f), t) * glm::mat4_cast(r) * glm::scale(glm::mat4(1.0f), s);
}

void StoreColumnMajor(const glm::mat4& matrix, float* out)
{
    std::memcpy(out, glm::value_ptr(matrix), 16 * sizeof(float));
}

void BuildNodeWorldMatrix(const SkeletonData& skeleton, PoseSampleWorkspace& workspace, uint32 nodeIndex)
{
    if (nodeIndex >= skeleton.BoneCount || workspace.Built[nodeIndex])
        return;
    const int32 parentIndex = nodeIndex < skeleton.Parent.size() ? skeleton.Parent[nodeIndex] : -1;
    if (parentIndex >= 0)
    {
        BuildNodeWorldMatrix(skeleton, workspace, static_cast<uint32>(parentIndex));
        workspace.World[nodeIndex] = workspace.World[static_cast<uint32>(parentIndex)] * workspace.Local[nodeIndex];
    }
    else
        workspace.World[nodeIndex] = workspace.Local[nodeIndex];
    workspace.Built[nodeIndex] = 1u;
}

} // namespace

void BlendLocalPoses(const SkeletonData& skeleton,
                     PoseSampleWorkspace& current,
                     const PoseSampleWorkspace& previous,
                     float32 alpha,
                     const BoneEvaluationPlan* plan)
{
    const uint32 count = static_cast<uint32>(std::min(current.Pose.size(), previous.Pose.size()));
    const bool reduced = plan && plan->IsForSkeleton(skeleton);
    const float weight = std::clamp(alpha, 0.0f, 1.0f);
    for (uint32 node = 0; node < count; ++node)
    {
        if (reduced && node < skeleton.BoneCount && plan->EvaluatedAncestors()[node] != node)
            continue;
        current.Pose[node].t = glm::mix(previous.Pose[node].t, current.Pose[node].t, weight);
        current.Pose[node].r = glm::slerp(previous.Pose[node].r, current.Pose[node].r, weight);
        current.Pose[node].s = glm::mix(previous.Pose[node].s, current.Pose[node].s, weight);
        if (node < current.ChangedTranslation.size())
            current.ChangedTranslation[node] = 1u;
        if (node < current.ChangedRotation.size())
            current.ChangedRotation[node] = 1u;
        if (node < current.ChangedScale.size())
            current.ChangedScale[node] = 1u;
    }
}

void BuildPoseToSkinMatrices(const SkeletonData& skeleton,
                             std::vector<float>* outNodeWorldMatrices,
                             std::vector<float>* outCompactSkinMatrices,
                             PoseSampleWorkspace& workspace,
                             const BoneEvaluationPlan* plan)
{
    const uint32_t boneCount = skeleton.BoneCount;
    const bool reduced = plan && plan->IsForSkeleton(skeleton);

    workspace.Local.clear();
    workspace.Local.resize(boneCount, glm::mat4(1.0f));
    const bool hasRestLocal = skeleton.RestLocalMatrix.size() >= static_cast<size_t>(boneCount) * 16u;
    for (uint32_t nodeIndex = 0; nodeIndex < boneCount; ++nodeIndex)
    {
        if (reduced && plan->EvaluatedAncestors()[nodeIndex] != nodeIndex)
            continue;
        const bool anyChanged = (workspace.ChangedTranslation[nodeIndex] | workspace.ChangedRotation[nodeIndex] | workspace.ChangedScale[nodeIndex]) != 0u;
        if (hasRestLocal && !anyChanged)
            workspace.Local[nodeIndex] = glm::make_mat4(&skeleton.RestLocalMatrix[nodeIndex * 16u]);
        else
            workspace.Local[nodeIndex] = BuildLocalTRS(workspace.Pose[nodeIndex].t, workspace.Pose[nodeIndex].r, workspace.Pose[nodeIndex].s);
    }

    workspace.World.clear();
    workspace.World.resize(boneCount, glm::mat4(1.0f));
    workspace.Built.clear();
    workspace.Built.resize(boneCount, uint8_t{0});

    if (reduced)
    {
        const auto evaluatedNodes = outNodeWorldMatrices ? plan->Nodes() : plan->SkinNodes();
        for (uint32 node : evaluatedNodes)
        {
            const int32 parent = skeleton.Parent[node];
            workspace.World[node] = parent >= 0
                ? workspace.World[static_cast<uint32>(parent)] * workspace.Local[node]
                : workspace.Local[node];
        }
        if (outNodeWorldMatrices)
        {
            for (uint32 node = 0; node < boneCount; ++node)
            {
                const uint32 ancestor = plan->EvaluatedAncestors()[node];
                if (ancestor != node)
                    workspace.World[node] = workspace.World[ancestor] * plan->RestFromAncestors()[node].GetGLM();
            }
        }
    }
    else
    {
        for (uint32_t nodeIndex = 0; nodeIndex < boneCount; ++nodeIndex)
            BuildNodeWorldMatrix(skeleton, workspace, nodeIndex);
    }

    if (outNodeWorldMatrices)
    {
        outNodeWorldMatrices->resize(static_cast<size_t>(boneCount) * 16u);
        for (uint32_t nodeIndex = 0; nodeIndex < boneCount; ++nodeIndex)
            StoreColumnMajor(workspace.World[nodeIndex], outNodeWorldMatrices->data() + static_cast<size_t>(nodeIndex) * 16u);
    }

    if (!outCompactSkinMatrices)
        return;

    const uint32_t jointCount =
        (skeleton.SkinJointCount > 0 && skeleton.JointNodes.size() == skeleton.SkinJointCount)
            ? skeleton.SkinJointCount
            : boneCount;
    outCompactSkinMatrices->resize(static_cast<size_t>(jointCount) * 16u);

    const glm::mat4 meshRoot = glm::make_mat4(skeleton.MeshRootWorld);
    const glm::mat4 meshRootInverse = glm::inverse(meshRoot);
    for (uint32_t jointIndex = 0; jointIndex < jointCount; ++jointIndex)
    {
        const uint32_t nodeIndex = (jointIndex < skeleton.JointNodes.size()) ? skeleton.JointNodes[jointIndex] : jointIndex;
        if (nodeIndex >= boneCount)
            continue;

        const uint32 evaluatedNode = reduced ? plan->EvaluatedAncestors()[nodeIndex] : nodeIndex;
        const glm::mat4 inverseBind = reduced ? plan->JointCorrections()[jointIndex].GetGLM()
            : glm::make_mat4(&skeleton.InverseBind[static_cast<size_t>(nodeIndex) * 16u]);
        const glm::mat4 modelRelative = meshRootInverse * workspace.World[evaluatedNode];
        const glm::mat4 skinMatrix = modelRelative * inverseBind;
        StoreColumnMajor(skinMatrix, outCompactSkinMatrices->data() + static_cast<size_t>(jointIndex) * 16u);
    }
}

void EmitSkinMatricesFromAnimationPose(const SkeletonData& skeleton,
                                       const AnimationPose& localPose,
                                       std::vector<float>& outCompactSkinMatrices,
                                       PoseSampleWorkspace& workspace,
                                       const BoneEvaluationPlan* plan)
{
    const uint32_t boneCount = skeleton.BoneCount;
    if (boneCount == 0)
    {
        outCompactSkinMatrices.clear();
        return;
    }

    workspace.Pose.assign(boneCount, PoseSampleWorkspace::TRS{});
    const bool reduced = plan && plan->IsForSkeleton(skeleton);
    for (uint32_t nodeIndex = 0; nodeIndex < boneCount && nodeIndex < localPose.BoneCount; ++nodeIndex)
    {
        if (reduced && plan->EvaluatedAncestors()[nodeIndex] != nodeIndex)
            continue;
        const auto& position = localPose.Positions[nodeIndex];
        const auto& rotation = localPose.Rotations[nodeIndex].GetGLM();
        const auto& scale = localPose.Scales[nodeIndex];
        workspace.Pose[nodeIndex].t = glm::vec3(position.x, position.y, position.z);
        workspace.Pose[nodeIndex].r = rotation;
        workspace.Pose[nodeIndex].s = glm::vec3(scale.x, scale.y, scale.z);
    }
    workspace.ChangedTranslation.assign(boneCount, 1u);
    workspace.ChangedRotation.assign(boneCount, 1u);
    workspace.ChangedScale.assign(boneCount, 1u);

    BuildPoseToSkinMatrices(skeleton, nullptr, &outCompactSkinMatrices, workspace, plan);
}

} // namespace Animation
} // namespace GameEngine
