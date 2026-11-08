#pragma once

#include "Mathematics/Matrix4x4.h"
#include "Types/Types.h"

#include <span>
#include <vector>

namespace GameEngine::Animation
{

struct SkeletonData;

/// An ancestor-closed set of evaluated nodes and rest transforms for omitted nodes.
/// Node indices and the skin-joint palette order remain unchanged.
class BoneEvaluationPlan
{
public:
    /// Build from a skeleton with computed level topology. Roots and every
    /// requested node's ancestors are retained. Failure clears the plan.
    bool Build(const SkeletonData& skeleton, std::span<const uint32> requestedNodes);

    /// The skeleton must keep its address and authored data until the plan is rebuilt.
    bool IsForSkeleton(const SkeletonData& skeleton) const;
    std::span<const uint32> Nodes() const { return m_Nodes; }
    std::span<const uint32> LevelOffsets() const { return m_LevelOffsets; }
    std::span<const uint32> EvaluatedAncestors() const { return m_EvaluatedAncestors; }
    std::span<const Mathematics::Matrix4x4> RestFromAncestors() const { return m_RestFromAncestors; }
    /// Skin-joint order: omitted rest chain times that joint's inverse bind.
    std::span<const Mathematics::Matrix4x4> JointCorrections() const { return m_JointCorrections; }

private:
    void Clear();

    const SkeletonData* m_Skeleton = nullptr;
    std::vector<uint32> m_Nodes;
    std::vector<uint32> m_LevelOffsets;
    std::vector<uint32> m_EvaluatedAncestors;
    std::vector<Mathematics::Matrix4x4> m_RestFromAncestors;
    std::vector<Mathematics::Matrix4x4> m_JointCorrections;
};

} // namespace GameEngine::Animation
