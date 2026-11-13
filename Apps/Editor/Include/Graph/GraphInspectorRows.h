#pragma once

#include "Graph/GraphNode/GraphNodeRegistry.h"

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace GameEngine {

class UIElement;
struct GraphTransitionDesc;

enum class GraphInspectorRowKind
{
    VariableName,
    ColorSwatch,
    VariableColor,
    VariableFloat,
    VariableVec,
    NodeInt,
    NodeFloat,
    NodeText,
    NodeEnum,
    NodeAsset
};

struct GraphInspectorRow
{
    GraphInspectorRowKind Kind = GraphInspectorRowKind::NodeText;
    std::string Label;
    std::string Key;
    int VecAxis = -1;
    int VecCount = 0;
    /* NodeAsset rows: the asset type the field accepts. */
    AssetType AssetReference = AssetType::Unknown;
};

/** Null when `typeId` is missing or registered under more than one kind. */
const NodeTypeMeta* FindUniqueNodeTypeMeta(std::string_view typeId);
/** The schema's spec for one parameter id; null without a schema or a match. */
const NodeParamSpec* FindSchemaParam(const NodeTypeMeta* schema, std::string_view id);
/** KindId lookup when the open graph is known; otherwise FindUnique. */
const NodeTypeMeta* FindNodeTypeMeta(std::string_view kindId, std::string_view typeId);

std::vector<GraphInspectorRow> PlanGraphInspectorRows(
    const std::unordered_map<std::string, std::string>& parameters,
    const NodeTypeMeta* schema);

/** Adds the Conditions section's state to `conditions`: a warning notice when `transition` has no condition, because
    compile skips such a transition. A notice, not an info card, so the "Show Info Cards" switch does not hide it. */
void AddGraphTransitionConditionsNotice(UIElement* conditions, const GraphTransitionDesc& transition);

} // namespace GameEngine
