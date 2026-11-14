#pragma once

#include <nlohmann/json.hpp>

#include <functional>
#include <string>

namespace GameEngine
{
class DockingManager;

namespace Editor
{
/** Validates and moves a dock panel before the handler waits for layout readback. */
class DockPanelRequest
{
public:
    explicit DockPanelRequest(const nlohmann::json& params);

    const std::string& Error() const { return m_Error; }

    /** Opens closed panels through the editor's existing placement policy. */
    bool Apply(DockingManager& docking,
               const std::function<void(const std::string&, const std::string&)>& showPanel);

    /** Reads current placement and dimensions after the layout has settled. */
    nlohmann::json Report(const DockingManager& docking) const;

private:
    std::string m_PanelId;
    std::string m_AnchorPanelId;
    std::string m_LeafBefore;
    std::string m_Error;
    bool m_Moved = false;
};
} // namespace Editor
} // namespace GameEngine
