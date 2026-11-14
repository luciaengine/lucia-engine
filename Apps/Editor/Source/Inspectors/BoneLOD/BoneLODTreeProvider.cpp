#include "Inspectors/BoneLOD/BoneLODTreeProvider.h"

#include "Animation/BoneEvaluationPlan.h"
#include "Animation/Skeleton/SkeletonData.h"
#include "Types/StringUtils.h"

namespace GameEngine::Editor
{

BoneLODTreeProvider::BoneLODTreeProvider(const Animation::SkeletonData& skeleton)
{
    Animation::BoneEvaluationPlan validation;
    if (!validation.Build(skeleton, {}))
        return;
    m_Parents = skeleton.Parent;
    m_Order = skeleton.TopologicalOrder;
    m_Names = skeleton.BoneNames;
    m_Names.resize(skeleton.BoneCount);
    m_Children.resize(skeleton.BoneCount);
    m_Visible.assign(skeleton.BoneCount, 1u);
    for (uint32 node : m_Order)
    {
        const int32 parent = m_Parents[node];
        if (parent < 0)
            m_Roots.push_back(node + 1u);
        else
            m_Children[static_cast<uint32>(parent)].push_back(node + 1u);
    }
}

int BoneLODTreeProvider::GetRootCount() const { return static_cast<int>(m_Roots.size()); }
TreeId BoneLODTreeProvider::GetRootId(int index) const { return m_Roots.at(static_cast<size_t>(index)); }
int BoneLODTreeProvider::GetChildCount(TreeId parent) const
{
    return parent > 0u && parent <= m_Children.size() ? static_cast<int>(m_Children[parent - 1u].size()) : 0;
}
TreeId BoneLODTreeProvider::GetChildId(TreeId parent, int index) const
{
    return m_Children.at(parent - 1u).at(static_cast<size_t>(index));
}
const char* BoneLODTreeProvider::GetLabel(TreeId id) const
{
    if (id == 0u || id > m_Names.size())
        return "";
    return m_Names[id - 1u].empty() ? "(unnamed)" : m_Names[id - 1u].c_str();
}
bool BoneLODTreeProvider::IsExpandable(TreeId id) const { return GetChildCount(id) > 0; }

std::vector<std::string> BoneLODTreeProvider::Path(TreeId id) const
{
    std::vector<std::string> path;
    if (id == 0u || id > m_Parents.size())
        return path;
    int32 node = static_cast<int32>(id - 1u);
    while (node >= 0)
    {
        path.push_back(m_Names[static_cast<uint32>(node)]);
        node = m_Parents[static_cast<uint32>(node)];
    }
    std::reverse(path.begin(), path.end());
    return path;
}

void BoneLODTreeProvider::SetSearch(const std::string& text)
{
    for (size_t node = 0u; node < m_Names.size(); ++node)
        m_Visible[node] = ContainsIgnoreCase(m_Names[node], text) ? 1u : 0u;
    for (auto node = m_Order.rbegin(); node != m_Order.rend(); ++node)
    {
        if (m_Visible[*node] != 0u && m_Parents[*node] >= 0)
            m_Visible[static_cast<uint32>(m_Parents[*node])] = 1u;
    }
    MarkStructureChanged();
}

bool BoneLODTreeProvider::IsVisible(TreeId id) const
{
    return id > 0u && id <= m_Visible.size() && m_Visible[id - 1u] != 0u;
}

} // namespace GameEngine::Editor
