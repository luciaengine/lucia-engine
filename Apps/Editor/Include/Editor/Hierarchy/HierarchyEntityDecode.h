#pragma once

#include "ECS/ECS.h"
#include "Editor/DragDropPayloads.h"
#include "UI/Interaction/DropTarget.h"
#include "UI/Interaction/Payload.h"

#include <cstdint>
#include <vector>

namespace GameEngine::Editor
{

/// Decodes every packed entity handle carried by a hierarchy drag.
inline std::vector<ECS::EntityHandle> DecodeHierarchyEntities(const UI::Interaction::DropRequest& request)
{
    std::vector<ECS::EntityHandle> entities;
    const auto* payload = request.payload.TryGet<HierarchyEntityDragPayload>();
    if (!payload)
        return entities;
    entities.reserve(payload->treeIds.size());
    for (const auto treeId : payload->treeIds)
        entities.emplace_back(static_cast<std::uint32_t>(treeId));
    return entities;
}

// Decode the first entity of a HierarchyEntityDragPayload. Mirrors
// HierarchyDataProvider::Encode: raw = index | (version << kEntityIndexBits).
// The single decode shared by every drop target that accepts hierarchy drags.
inline ECS::EntityHandle DecodeFirstHierarchyEntity(const UI::Interaction::DropRequest& request)
{
    const auto* payload = request.payload.TryGet<HierarchyEntityDragPayload>();
    if (!payload || payload->treeIds.empty())
        return ECS::EntityHandle();
    const std::uint32_t raw = static_cast<std::uint32_t>(payload->treeIds[0]);
    const ECS::EntityIndex idx = static_cast<ECS::EntityIndex>(raw & ECS::kEntityIndexMask);
    const ECS::EntityVersion ver =
        static_cast<ECS::EntityVersion>((raw >> ECS::kEntityIndexBits) & ECS::kEntityVersionMask);
    return ECS::EntityHandle(idx, ver);
}

} // namespace GameEngine::Editor
