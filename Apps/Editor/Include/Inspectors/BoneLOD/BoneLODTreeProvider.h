#pragma once

#include "UI/Controls/TreeView.h"

namespace GameEngine::Animation { struct SkeletonData; }

namespace GameEngine::Editor
{

/// Owns an inspector snapshot so a model reload cannot invalidate tree rows.
class BoneLODTreeProvider final : public TreeChangeTrackingProvider
{
public:
    explicit BoneLODTreeProvider(const Animation::SkeletonData& skeleton);

    int GetRootCount() const override;
    TreeId GetRootId(int index) const override;
    int GetChildCount(TreeId parent) const override;
    TreeId GetChildId(TreeId parent, int index) const override;
    const char* GetLabel(TreeId id) const override;
    bool IsExpandable(TreeId id) const override;

    std::vector<std::string> Path(TreeId id) const;
    void SetSearch(const std::string& text);
    bool IsVisible(TreeId id) const;

private:
    std::vector<std::string> m_Names;
    std::vector<int32> m_Parents;
    std::vector<uint32> m_Order;
    std::vector<TreeId> m_Roots;
    std::vector<std::vector<TreeId>> m_Children;
    std::vector<uint8> m_Visible;
};

} // namespace GameEngine::Editor
