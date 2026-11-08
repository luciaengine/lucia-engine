#include "Animation/BoneEvaluationPlan.h"

#include "Animation/SkeletonData.h"

#include <algorithm>
#include <cmath>

namespace GameEngine::Animation
{

namespace
{

bool IsFiniteMatrix(const Mathematics::Matrix4x4& matrix)
{
    return std::all_of(matrix.Data(), matrix.Data() + 16u,
        [](float value) { return std::isfinite(value); });
}

bool HasValidTopology(const SkeletonData& skeleton, std::vector<uint32>& levels)
{
    const uint32 count = skeleton.BoneCount;
    if (skeleton.Parent.size() != count || skeleton.TopologicalOrder.size() != count ||
        skeleton.RestLocalMatrix.size() != static_cast<size_t>(count) * 16u ||
        skeleton.InverseBind.size() < static_cast<size_t>(count) * 16u)
        return false;

    std::vector<uint8> visited(count, 0u);
    levels.assign(count, 0u);
    uint32 previousLevel = 0u;
    for (uint32 node : skeleton.TopologicalOrder)
    {
        if (node >= count || visited[node] != 0u)
            return false;
        const int32 parent = skeleton.Parent[node];
        if (parent < -1 || (parent >= 0 &&
            (static_cast<uint32>(parent) >= count || visited[static_cast<uint32>(parent)] == 0u)))
            return false;
        if (parent >= 0)
            levels[node] = levels[static_cast<uint32>(parent)] + 1u;
        if (levels[node] < previousLevel)
            return false;
        previousLevel = levels[node];
        visited[node] = 1u;
    }
    return std::all_of(skeleton.RestLocalMatrix.begin(), skeleton.RestLocalMatrix.end(),
        [](float value) { return std::isfinite(value); }) &&
        std::all_of(skeleton.InverseBind.begin(), skeleton.InverseBind.end(),
        [](float value) { return std::isfinite(value); });
}

} // namespace

void BoneEvaluationPlan::Clear()
{
    m_Skeleton = nullptr;
    m_Nodes.clear();
    m_LevelOffsets.clear();
    m_EvaluatedAncestors.clear();
    m_RestFromAncestors.clear();
    m_JointCorrections.clear();
}

bool BoneEvaluationPlan::Build(const SkeletonData& skeleton, std::span<const uint32> requestedNodes)
{
    Clear();

    std::vector<uint32> levels;
    if (!HasValidTopology(skeleton, levels) ||
        std::any_of(requestedNodes.begin(), requestedNodes.end(),
            [&skeleton](uint32 node) { return node >= skeleton.BoneCount; }))
        return false;

    const uint32 count = skeleton.BoneCount;
    const uint32 jointCount = skeleton.SkinJointCount > 0u && skeleton.JointNodes.size() == skeleton.SkinJointCount
        ? skeleton.SkinJointCount : count;
    for (uint32 joint = 0; joint < jointCount; ++joint)
    {
        const uint32 node = joint < skeleton.JointNodes.size() ? skeleton.JointNodes[joint] : joint;
        if (node >= count)
            return false;
    }
    std::vector<uint8> active(count, 0u);
    for (uint32 node = 0; node < count; ++node)
    {
        if (skeleton.Parent[node] < 0)
            active[node] = 1u;
    }
    for (uint32 node : requestedNodes)
    {
        while (active[node] == 0u)
        {
            active[node] = 1u;
            const int32 parent = skeleton.Parent[node];
            if (parent < 0)
                break;
            node = static_cast<uint32>(parent);
        }
    }

    m_Nodes.reserve(count);
    m_LevelOffsets.push_back(0u);
    m_EvaluatedAncestors.resize(count);
    m_RestFromAncestors.resize(count);
    uint32 currentLevel = 0u;
    for (uint32 node : skeleton.TopologicalOrder)
    {
        if (active[node] != 0u)
        {
            if (levels[node] != currentLevel)
            {
                m_LevelOffsets.push_back(static_cast<uint32>(m_Nodes.size()));
                currentLevel = levels[node];
            }
            m_Nodes.push_back(node);
            m_EvaluatedAncestors[node] = node;
            m_RestFromAncestors[node] = Mathematics::Matrix4x4::Identity();
            continue;
        }

        const uint32 parent = static_cast<uint32>(skeleton.Parent[node]);
        m_EvaluatedAncestors[node] = m_EvaluatedAncestors[parent];
        m_RestFromAncestors[node] = m_RestFromAncestors[parent] *
            Mathematics::Matrix4x4::FromColumnMajor(skeleton.RestLocalMatrix.data() +
                static_cast<size_t>(node) * 16u);
    }
    if (count != 0u)
        m_LevelOffsets.push_back(static_cast<uint32>(m_Nodes.size()));
    m_JointCorrections.reserve(jointCount);
    for (uint32 joint = 0; joint < jointCount; ++joint)
    {
        const uint32 node = joint < skeleton.JointNodes.size() ? skeleton.JointNodes[joint] : joint;
        const Mathematics::Matrix4x4 inverseBind = Mathematics::Matrix4x4::FromColumnMajor(
            skeleton.InverseBind.data() + static_cast<size_t>(node) * 16u);
        m_JointCorrections.push_back(active[node] != 0u
            ? inverseBind : m_RestFromAncestors[node] * inverseBind);
    }
    if (!std::all_of(m_RestFromAncestors.begin(), m_RestFromAncestors.end(), IsFiniteMatrix) ||
        !std::all_of(m_JointCorrections.begin(), m_JointCorrections.end(), IsFiniteMatrix))
    {
        Clear();
        return false;
    }
    m_Skeleton = &skeleton;
    return true;
}

bool BoneEvaluationPlan::IsForSkeleton(const SkeletonData& skeleton) const
{
    return m_Skeleton == &skeleton && m_EvaluatedAncestors.size() == skeleton.BoneCount;
}

} // namespace GameEngine::Animation
