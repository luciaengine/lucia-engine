#pragma once

#include "ECS/Entity.h"
#include "Types/Types.h"
#include "UI/UIElement.h"

#include <functional>
#include <string>
#include <vector>

namespace GameEngine::EditorUI
{

/// The junctions on one spline: the other spline and where they meet in the inspector's label column,
/// the state and the fix action in its value column. The list is the whole of a section that names it.
/// The caller owns the data and the undo steps, and builds the list again after each gesture.
/// Layouts: SplineJunctionList.uxml, SplineJunctionRow.uxml.
class SplineJunctionList final : public UIElement
{
  public:
    /// One junction. `Title` names the other spline and where the two meet; `State` says what the
    /// junction is now, in the warning color when `Warning`. An empty `Action` shows no button.
    struct Row
    {
        uint32 NodeId = 0;
        ECS::EntityHandle Other{};
        std::string Title;
        std::string State;
        bool Warning = false;
        std::string Action;
        std::string ActionTooltip;
    };

    /// The rows in their order. While not `Editable`, no action runs.
    struct Content
    {
        std::string Empty;
        std::vector<Row> Rows;
        bool Editable = true;
    };

    /// `Act` is called with the row's junction on a click on its action; `Select` with the other
    /// spline on a click on the row's title.
    struct Handlers
    {
        std::function<void(uint32)> Act;
        std::function<void(ECS::EntityHandle)> Select;
    };

    SplineJunctionList(const Content& content, const Handlers& handlers);
};

} // namespace GameEngine::EditorUI
