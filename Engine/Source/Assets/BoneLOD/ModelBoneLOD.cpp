#include "Assets/BoneLOD/ModelBoneLOD.h"

#include "Animation/BoneEvaluationPlan.h"
#include "Animation/PoseToSkinMatrices.h"
#include "Assets/BoneLOD/BoneLODAssetSettings.h"
#include "Assets/ModelAsset.h"
#include "Mathematics/MatrixOps.h"
#include "Mathematics/Vector4.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>

namespace GameEngine
{

namespace
{

std::string_view NodeName(const Animation::SkeletonData& skeleton, uint32 node)
{
    return node < skeleton.BoneNames.size() ? std::string_view(skeleton.BoneNames[node]) : std::string_view{};
}

bool MatchesPath(const Animation::SkeletonData& skeleton, uint32 node, const std::vector<std::string>& path)
{
    for (auto segment = path.rbegin(); segment != path.rend(); ++segment)
    {
        if (NodeName(skeleton, node) != *segment)
            return false;
        const int32 parent = skeleton.Parent[node];
        if (segment + 1 == path.rend())
            return parent < 0;
        if (parent < 0)
            return false;
        node = static_cast<uint32>(parent);
    }
    return false;
}

bool ResolveMandatoryNodes(const Animation::SkeletonData& skeleton, const BoneLODAssetSettings& settings,
    std::vector<uint32>& nodes, bool& ambiguous)
{
    for (const auto& path : settings.MandatoryPaths)
    {
        uint32 matches = 0u;
        for (uint32 node = 0; node < skeleton.BoneCount; ++node)
        {
            if (MatchesPath(skeleton, node, path))
            {
                nodes.push_back(node);
                ++matches;
            }
        }
        if (matches == 0u)
            return false;
        ambiguous |= matches > 1u;
    }
    return true;
}

bool AddMeshSignificance(const Mesh& mesh, const Animation::SkeletonData& skeleton,
    const std::vector<Mathematics::Matrix4x4>& restToMesh, std::vector<float>& significance)
{
    std::vector<float> morphRadius(mesh.Vertices.size(), 0.0f);
    for (const MorphTarget& target : mesh.MorphTargets)
    {
        if (target.PositionDeltas.size() != target.VertexIndices.size() * 3u)
            return false;
        for (size_t entry = 0; entry < target.VertexIndices.size(); ++entry)
        {
            const uint32 vertex = target.VertexIndices[entry];
            if (vertex >= mesh.Vertices.size())
                return false;
            const Mathematics::Vector3 delta(target.PositionDeltas[entry * 3u],
                target.PositionDeltas[entry * 3u + 1u], target.PositionDeltas[entry * 3u + 2u]);
            morphRadius[vertex] += delta.Length();
        }
    }

    const uint32 jointCount = skeleton.SkinJointCount > 0u && skeleton.JointNodes.size() == skeleton.SkinJointCount
        ? skeleton.SkinJointCount : skeleton.BoneCount;
    const uint32 influenceCount = mesh.HasSkinning1() ? 8u : 4u;
    for (size_t vertex = 0; vertex < mesh.Vertices.size(); ++vertex)
    {
        const auto& position = mesh.Vertices[vertex].Position;
        const Mathematics::Vector3 point(position[0], position[1], position[2]);
        if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z) ||
            !std::isfinite(morphRadius[vertex]))
            return false;
        std::array<std::pair<uint32, float>, 8u> nodeWeights{};
        uint32 distinctNodes = 0u;
        for (uint32 influence = 0; influence < influenceCount; ++influence)
        {
            const bool second = influence >= 4u;
            const size_t offset = vertex * 4u + influence % 4u;
            const float weight = second ? mesh.Weights1[offset] : mesh.Weights0[offset];
            if (!std::isfinite(weight) || weight < 0.0f)
                return false;
            if (weight == 0.0f)
                continue;
            const uint32 joint = second ? mesh.Joints1[offset] : mesh.Joints0[offset];
            if (joint >= jointCount)
                return false;
            const uint32 node = joint < skeleton.JointNodes.size() ? skeleton.JointNodes[joint] : joint;
            auto found = std::find_if(nodeWeights.begin(), nodeWeights.begin() + distinctNodes,
                [node](const auto& entry) { return entry.first == node; });
            if (found != nodeWeights.begin() + distinctNodes)
                found->second += weight;
            else
                nodeWeights[distinctNodes++] = {node, weight};
        }
        for (uint32 entry = 0; entry < distinctNodes; ++entry)
        {
            const auto [node, weight] = nodeWeights[entry];
            const auto inverseBind = Mathematics::Matrix4x4::FromColumnMajor(
                skeleton.InverseBind.data() + static_cast<size_t>(node) * 16u);
            const auto localPoint = inverseBind.TransformPoint(point);
            const auto lever = restToMesh[node].Transform(Mathematics::Vector4(localPoint, 0.0f));
            const float impact = weight * (Mathematics::Vector3(lever.x, lever.y, lever.z).Length() + morphRadius[vertex]);
            if (!std::isfinite(impact))
                return false;
            significance[node] = std::max(significance[node], impact);
        }
    }
    return true;
}

bool ComputeSignificance(const ModelAsset& model, const Animation::SkeletonData& skeleton,
    const Animation::BoneEvaluationPlan& required, std::vector<float>& significance)
{
    Animation::PoseSampleWorkspace workspace;
    workspace.Pose.resize(skeleton.BoneCount);
    workspace.ChangedTranslation.assign(skeleton.BoneCount, 0u);
    workspace.ChangedRotation.assign(skeleton.BoneCount, 0u);
    workspace.ChangedScale.assign(skeleton.BoneCount, 0u);
    std::vector<float> nodeWorlds;
    Animation::BuildPoseToSkinMatrices(skeleton, &nodeWorlds, nullptr, workspace, &required);
    const auto meshRootInverse = Mathematics::Inverse(Mathematics::Matrix4x4::FromColumnMajor(skeleton.MeshRootWorld));
    if (!std::all_of(meshRootInverse.Data(), meshRootInverse.Data() + 16u,
        [](float value) { return std::isfinite(value); }))
        return false;
    std::vector<Mathematics::Matrix4x4> restToMesh;
    restToMesh.reserve(skeleton.BoneCount);
    for (uint32 node = 0; node < skeleton.BoneCount; ++node)
        restToMesh.push_back(meshRootInverse * Mathematics::Matrix4x4(workspace.World[node]));

    significance.assign(skeleton.BoneCount, 0.0f);
    bool haveSkin = false;
    for (const Mesh& mesh : model.GetMeshes())
    {
        if ((mesh.Skinned && !mesh.IsSkinned()) ||
            ((!mesh.Joints1.empty() || !mesh.Weights1.empty()) && !mesh.HasSkinning1()))
            return false;
        if (!mesh.IsSkinned())
            continue;
        haveSkin = true;
        if (!AddMeshSignificance(mesh, skeleton, restToMesh, significance))
            return false;
    }
    return haveSkin;
}

} // namespace

bool ModelBoneLOD::Build(const ModelAsset& model, const Animation::SkeletonData& skeleton,
    const BoneLODAssetSettings& settings)
{
    *this = ModelBoneLOD{};
    m_RequestedBoneBudget = settings.BoneBudget;
    m_ActualBoneCount = static_cast<uint32>(skeleton.JointClosure.size());
    if (!settings.Error.empty())
    {
        m_Error = settings.Error;
        return false;
    }
    Animation::BoneEvaluationPlan required;
    if (!required.Build(skeleton, {}))
    {
        m_Error = "Bone LOD requires a valid skeleton hierarchy, rest transforms, and inverse bind matrices.";
        return false;
    }
    bool ambiguous = false;
    std::vector<uint32> mandatory;
    if (!ResolveMandatoryNodes(skeleton, settings, mandatory, ambiguous) || !required.Build(skeleton, mandatory))
    {
        m_Error = "A mandatory bone path was not found. Update the model's mandatory bones to enable reduction.";
        return false;
    }
    m_RequiredBoneCount = static_cast<uint32>(required.SkinNodes().size());
    if (ambiguous)
        m_Warning = "Some mandatory bone paths match several nodes. Every match is retained.";
    if (settings.BoneBudget == 0u || settings.BoneBudget >= skeleton.JointClosure.size())
        return true;

    std::vector<float> significance;
    if (!ComputeSignificance(model, skeleton, required, significance))
    {
        m_Error = "Bone LOD requires valid skinned mesh influences and finite geometry.";
        return false;
    }
    std::vector<uint32> depth(skeleton.BoneCount, 0u);
    for (uint32 node : skeleton.TopologicalOrder)
    {
        const int32 parent = skeleton.Parent[node];
        if (parent >= 0)
            depth[node] = depth[static_cast<uint32>(parent)] + 1u;
    }
    for (auto node = skeleton.TopologicalOrder.rbegin(); node != skeleton.TopologicalOrder.rend(); ++node)
    {
        const int32 parent = skeleton.Parent[*node];
        if (parent >= 0)
            significance[static_cast<uint32>(parent)] = std::max(significance[static_cast<uint32>(parent)], significance[*node]);
    }
    std::vector<uint32> ranked = skeleton.TopologicalOrder;
    std::sort(ranked.begin(), ranked.end(), [&significance, &depth](uint32 first, uint32 second)
    {
        if (significance[first] != significance[second])
            return significance[first] > significance[second];
        if (depth[first] != depth[second])
            return depth[first] < depth[second];
        return first < second;
    });
    std::vector<uint8> selected(skeleton.BoneCount, 0u);
    std::vector<uint8> reachesSkin(skeleton.BoneCount, 0u);
    for (uint32 node : skeleton.JointClosure)
        reachesSkin[node] = 1u;
    for (uint32 node : required.Nodes())
    {
        selected[node] = 1u;
        m_SelectedNodes.push_back(node);
    }
    uint32 selectedSkinCount = m_RequiredBoneCount;
    for (uint32 node : ranked)
    {
        if (selectedSkinCount >= settings.BoneBudget)
            break;
        if (selected[node] == 0u && reachesSkin[node] != 0u)
        {
            selected[node] = 1u;
            m_SelectedNodes.push_back(node);
            ++selectedSkinCount;
        }
    }
    auto plan = MakeShared<Animation::BoneEvaluationPlan>();
    if (!plan->Build(skeleton, m_SelectedNodes))
    {
        m_Error = "Bone LOD could not build finite palette corrections. The full skeleton is retained.";
        return false;
    }
    m_ActualBoneCount = static_cast<uint32>(plan->SkinNodes().size());
    if (m_ActualBoneCount < skeleton.JointClosure.size())
        m_ReducedPlan = std::move(plan);
    return true;
}

SharedPtr<const ModelBoneLOD> ModelBoneLOD::Rebind(const Animation::SkeletonData& skeleton) const
{
    auto rebound = MakeShared<ModelBoneLOD>(*this);
    if (m_ReducedPlan)
    {
        auto plan = MakeShared<Animation::BoneEvaluationPlan>();
        if (plan->Build(skeleton, m_SelectedNodes))
            rebound->m_ReducedPlan = std::move(plan);
        else
        {
            rebound->m_ReducedPlan.reset();
            rebound->m_ActualBoneCount = static_cast<uint32>(skeleton.JointClosure.size());
            rebound->m_Error = "Bone LOD could not bind the reloaded skeleton. The full skeleton is retained.";
        }
    }
    return rebound;
}

} // namespace GameEngine
