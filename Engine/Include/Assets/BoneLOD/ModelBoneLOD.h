#pragma once

#include "Types/Types.h"

#include <string>
#include <vector>

namespace GameEngine
{

class ModelAsset;
struct BoneLODAssetSettings;
namespace Animation { class BoneEvaluationPlan; struct SkeletonData; }

/// Immutable model reduction data after Build. View changes reuse the same plan.
class ModelBoneLOD
{
public:
    bool Build(const ModelAsset& model, const Animation::SkeletonData& skeleton,
        const BoneLODAssetSettings& settings);
    /// Rebuild plan transforms after the same imported payload is copied to its live skeleton slot.
    SharedPtr<const ModelBoneLOD> Rebind(const Animation::SkeletonData& skeleton) const;

    const SharedPtr<const Animation::BoneEvaluationPlan>& ReducedPlan() const { return m_ReducedPlan; }
    uint32 RequestedBoneBudget() const { return m_RequestedBoneBudget; }
    uint32 RequiredBoneCount() const { return m_RequiredBoneCount; }
    uint32 ActualBoneCount() const { return m_ActualBoneCount; }
    const std::string& Error() const { return m_Error; }
    const std::string& Warning() const { return m_Warning; }

private:
    SharedPtr<const Animation::BoneEvaluationPlan> m_ReducedPlan;
    std::vector<uint32> m_SelectedNodes;
    uint32 m_RequestedBoneBudget = 0u;
    uint32 m_RequiredBoneCount = 0u;
    uint32 m_ActualBoneCount = 0u;
    std::string m_Error;
    std::string m_Warning;
};

} // namespace GameEngine
