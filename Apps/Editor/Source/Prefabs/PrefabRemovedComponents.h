#pragma once

#include "UI/UIElement.h"

#include <functional>
#include <string>
#include <vector>

namespace GameEngine
{
struct OverrideMenuTarget;
}

namespace GameEngine::Editor
{

/// The Inspector's headers, after an instance entity's components, for the components the prefab
/// gives the entity and the instance removed: one muted header each, with the component's icon, the
/// override edge and the Removed badge, and no options. A right-click on a header opens its override menu.
class PrefabRemovedComponents final : public UIElement
{
  public:
    /// One header for each scene token in `components`, in that order. `entity` names the entity and the
    /// editor services of the override menus; `refresh` runs after a menu entry.
    PrefabRemovedComponents(const std::vector<std::string>& components, const OverrideMenuTarget& entity,
                            const std::function<void()>& refresh);
};

} // namespace GameEngine::Editor
