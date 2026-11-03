#pragma once

#include "ECS/ECS.h"
#include "Types/Types.h"

#include <cmath>

namespace GameEngine::Engine::Renderer
{
/// Camera-independent metadata captured by extraction and shared by artwork renderers.
struct RenderSortingData
{
    int32 Layer = 0;
    int32 Order = 0;
    ECS::EntityHandle Group{};
    float32 GroupPosition[3]{};
    int32 MemberOrder = 0;
    bool Authored = false;
    bool Grouped = false;
};
struct RenderSortKey
{
    int32 Layer = 0;
    int32 Order = 0;
    float32 Depth = 0;
    ECS::EntityHandle Group{};
    int32 MemberOrder = 0;
    float32 MemberDepth = 0;
    bool Grouped = false;
    bool operator==(const RenderSortKey&) const = default;
};
inline float32 FiniteRenderDepth(float32 depth) { return std::isfinite(depth) ? depth : 0.0f; }
/// The outer group supplies depth; members retain their own view depth inside the group.
inline RenderSortKey MakeRenderSortKey(const RenderSortingData& data, float32 viewDepth,
                                       const float32* cameraPosition, const float32* cameraForward)
{
    RenderSortKey key;
    key.Layer = data.Layer;
    key.Order = data.Order;
    key.Depth = FiniteRenderDepth(viewDepth);
    key.Group = data.Group;
    key.Grouped = data.Grouped;
    if (data.Grouped)
    {
        key.Depth = FiniteRenderDepth((data.GroupPosition[0] - cameraPosition[0]) * cameraForward[0] +
                                     (data.GroupPosition[1] - cameraPosition[1]) * cameraForward[1] +
                                     (data.GroupPosition[2] - cameraPosition[2]) * cameraForward[2]);
        key.MemberOrder = data.MemberOrder;
        key.MemberDepth = FiniteRenderDepth(viewDepth);
    }
    return key;
}
inline bool RenderSortBefore(const RenderSortKey& a, const RenderSortKey& b)
{
    if (a.Layer != b.Layer) return a.Layer < b.Layer;
    if (a.Order != b.Order) return a.Order < b.Order;
    if (a.Depth != b.Depth) return a.Depth > b.Depth;
    if (a.Grouped != b.Grouped) return a.Grouped < b.Grouped;
    if (a.Grouped && a.Group.id != b.Group.id) return a.Group.id < b.Group.id;
    if (a.MemberOrder != b.MemberOrder) return a.MemberOrder < b.MemberOrder;
    return a.MemberDepth > b.MemberDepth;
}
}
