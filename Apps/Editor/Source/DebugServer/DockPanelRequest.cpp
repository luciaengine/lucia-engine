#include "DebugServer/DockPanelRequest.h"

#include "UI/Layout/Docking.h"
#include "UI/UIElement.h"

namespace GameEngine::Editor
{
DockPanelRequest::DockPanelRequest(const nlohmann::json& params)
{
    if (!params.is_object() || !params.contains("panelId") ||
        !params["panelId"].is_string() || params["panelId"].get_ref<const std::string&>().empty())
    {
        m_Error = "Provide a nonempty string panelId";
        return;
    }
    m_PanelId = params["panelId"].get<std::string>();
    if (params.contains("anchorPanelId"))
    {
        if (!params["anchorPanelId"].is_string() ||
            params["anchorPanelId"].get_ref<const std::string&>().empty())
        {
            m_Error = "anchorPanelId must be a nonempty string when provided";
            return;
        }
        m_AnchorPanelId = params["anchorPanelId"].get<std::string>();
    }
}

bool DockPanelRequest::Apply(
    DockingManager& docking,
    const std::function<void(const std::string&, const std::string&)>& showPanel)
{
    if (!m_Error.empty())
        return false;
    if (!docking.GetPanels().contains(m_PanelId))
    {
        m_Error = "panelId is not a registered editor panel";
        return false;
    }
    std::string anchorLeaf;
    if (!m_AnchorPanelId.empty() && !docking.FindLeafContaining(m_AnchorPanelId, anchorLeaf))
    {
        m_Error = "anchorPanelId must name a panel in the main dock tree";
        return false;
    }
    const bool wasDocked = docking.FindLeafContaining(m_PanelId, m_LeafBefore) != nullptr;
    m_Moved = wasDocked && !m_AnchorPanelId.empty() && m_LeafBefore != anchorLeaf;
    if (m_Moved)
    {
        docking.RemoveTabForDockMove(m_PanelId);
        // Removing the last source tab can collapse a split, changing the anchor's path.
        docking.FindLeafContaining(m_AnchorPanelId, anchorLeaf);
        docking.DockAsTabInLeafByPath(anchorLeaf, m_PanelId);
        docking.ActivateTab(m_PanelId);
    }
    else if (wasDocked)
    {
        // Same-leaf and no-anchor requests preserve tab order and placement.
        docking.ActivateTab(m_PanelId);
    }
    else
    {
        showPanel(m_PanelId, m_AnchorPanelId);
    }
    std::string leaf;
    if (!docking.FindLeafContaining(m_PanelId, leaf))
    {
        m_Error = "panel is not in the main dock tree after the request";
        return false;
    }
    return true;
}

nlohmann::json DockPanelRequest::Report(const DockingManager& docking) const
{
    std::string leaf;
    const bool docked = docking.FindLeafContaining(m_PanelId, leaf) != nullptr;
    const UIElement* panel = docking.GetPanel(m_PanelId);
    nlohmann::json result{{"panelId", m_PanelId},
                           {"anchorPanelId", m_AnchorPanelId},
                           {"docked", docked},
                           {"leaf", leaf},
                           {"activeTab", docking.IsPanelActiveTab(m_PanelId)},
                           {"width", panel ? panel->GetLayoutWidth() : 0.0f},
                           {"height", panel ? panel->GetLayoutHeight() : 0.0f}};
    if (m_Moved)
        result["movedFrom"] = m_LeafBefore;
    return result;
}
} // namespace GameEngine::Editor
