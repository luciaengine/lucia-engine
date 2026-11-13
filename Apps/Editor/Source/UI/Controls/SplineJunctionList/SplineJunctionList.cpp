#include "UI/Controls/SplineJunctionList/SplineJunctionList.h"

#include "Inspectors/InspectorDragHelpers.h"
#include "Logger/Logger.h"
#include "UI/ControlLayoutLoader.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Label.h"
#include "UI/UIEvents.h"

#include <memory>
#include <utility>

namespace GameEngine::EditorUI
{
namespace
{

using Content = SplineJunctionList::Content;
using Handlers = SplineJunctionList::Handlers;
using Row = SplineJunctionList::Row;

constexpr const char* kLayoutAssetPath = "UI/controls/SplineJunctionList/SplineJunctionList.uxml";
constexpr const char* kRowLayoutAssetPath = "UI/controls/SplineJunctionList/SplineJunctionRow.uxml";
constexpr const char* kStyleAssetPath = "UI/controls/SplineJunctionList/SplineJunctionList.css";

// core.css: display none.
constexpr const char* kHiddenClass = "hidden";

struct RowParts
{
    Label* Title = nullptr;
    Label* State = nullptr;
    Button* Action = nullptr;

    bool Complete() const { return Title && State && Action; }
};

RowParts FindRowParts(UIElement& row)
{
    RowParts parts;
    UIElement* name = InspectorDrag::FindChildByClass(&row, "spline-junction-name");
    UIElement* value = InspectorDrag::FindChildByClass(&row, "spline-junction-value");
    parts.Title = dynamic_cast<Label*>(InspectorDrag::FindChildByClass(name, "spline-junction-title"));
    parts.State = dynamic_cast<Label*>(InspectorDrag::FindChildByClass(value, "spline-junction-state"));
    parts.Action = dynamic_cast<Button*>(InspectorDrag::FindChildByClass(value, "spline-junction-action"));
    return parts;
}

void ShowTitle(Label& title, const Row& row, const Handlers& handlers)
{
    title.SetText(row.Title);
    title.SetTooltip("Click to select it");
    title.RegisterEventHandler(kEventMouseUp, [select = handlers.Select, other = row.Other](UIEvent& event) {
        if (event.Button == 0 && select)
            select(other);
    });
}

void ShowAction(Button& action, const Row& row, bool editable, const Handlers& handlers)
{
    if (row.Action.empty())
    {
        action.AddClass(kHiddenClass);
        return;
    }
    action.SetText(row.Action);
    action.SetTooltip(row.ActionTooltip);
    action.SetEnabled(editable);
    action.RegisterEventHandler(kEventButtonClick, [act = handlers.Act, node = row.NodeId](UIEvent&) {
        if (act)
            act(node);
    });
}

// The row of one junction, or null with an error when the row layout does not load or lacks a part.
std::unique_ptr<UIElement> BuildRow(const Row& row, bool editable, const Handlers& handlers)
{
    std::unique_ptr<UIElement> element = Editor::ControlLayoutLoader::Load(kRowLayoutAssetPath, {});
    if (!element)
        return nullptr;
    const RowParts parts = FindRowParts(*element);
    if (!parts.Complete())
    {
        Logger::Log::Error("SplineJunctionList: {} needs the title, the state and the action.", kRowLayoutAssetPath);
        return nullptr;
    }
    ShowTitle(*parts.Title, row, handlers);
    parts.State->SetText(row.State);
    if (row.Warning)
        parts.State->AddClass("spline-junction-state-warning");
    ShowAction(*parts.Action, row, editable, handlers);
    return element;
}

} // namespace

SplineJunctionList::SplineJunctionList(const Content& content, const Handlers& handlers)
{
    AddClass("spline-junction-group");
    RequestSubtreeStyleAssetPath(kStyleAssetPath, "editor");

    std::unique_ptr<UIElement> header = Editor::ControlLayoutLoader::Load(kLayoutAssetPath, {});
    if (!header)
        return;
    auto* empty = dynamic_cast<Label*>(InspectorDrag::FindChildByClass(header.get(), "spline-junction-empty"));
    if (!empty)
    {
        Logger::Log::Error("SplineJunctionList: {} needs the empty text.", kLayoutAssetPath);
        return;
    }
    empty->SetText(content.Empty);
    if (!content.Rows.empty())
        empty->AddClass(kHiddenClass);
    AddChild(std::move(header));
    for (const Row& row : content.Rows)
    {
        if (std::unique_ptr<UIElement> element = BuildRow(row, content.Editable, handlers))
            AddChild(std::move(element));
    }
}

} // namespace GameEngine::EditorUI
