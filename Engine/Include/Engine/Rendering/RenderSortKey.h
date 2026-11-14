#pragma once

#include "ECS/ECS.h"
#include "Mathematics/Vector3.h"
#include "Mathematics/WorldPosition.h"
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
    Mathematics::WorldPosition GroupPosition;
    int32 MemberOrder = 0;
    bool Authored = false;
    bool Grouped = false;
    bool SortByY = false;
    float32 SortOriginY = 0.0f;
    float64 SortY = 0.0;
    bool GroupSortByY = false;
};
struct RenderSortKey
{
    int32 Layer = 0;
    int32 Order = 0;
    float64 Depth = 0;
    ECS::EntityHandle Group{};
    int32 MemberOrder = 0;
    float64 MemberDepth = 0;
    bool Grouped = false;
    bool operator==(const RenderSortKey&) const = default;
};
inline float64 FiniteRenderDepth(float64 depth) { return std::isfinite(depth) ? depth : 0.0; }
/// The outer group supplies depth. Y sorting uses world ground height, with +Y up.
inline RenderSortKey MakeRenderSortKey(const RenderSortingData& data, float32 viewDepth,
                                       const Mathematics::WorldPosition& cameraPosition, const float32* cameraForward)
{
    RenderSortKey key;
    key.Layer = data.Layer;
    key.Order = data.Order;
    const float64 memberDepth = FiniteRenderDepth(data.SortByY ? data.SortY : viewDepth);
    key.Depth = memberDepth;
    key.Group = data.Group;
    key.Grouped = data.Grouped;
    if (data.Grouped)
    {
        const Mathematics::Vector3 offset = data.GroupPosition - cameraPosition;
        key.Depth = FiniteRenderDepth(data.GroupSortByY ? data.GroupPosition.Y :
            static_cast<float64>(offset.x) * cameraForward[0] +
            static_cast<float64>(offset.y) * cameraForward[1] +
            static_cast<float64>(offset.z) * cameraForward[2]);
        key.MemberOrder = data.MemberOrder;
        key.MemberDepth = memberDepth;
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
