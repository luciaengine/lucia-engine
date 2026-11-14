#pragma once
#include <memory_resource>

#include "Engine/Rendering/SortedTransparentCommand.h"
#include "Engine/Rendering/SortedTransparentRuns.h"

namespace GameEngine::Engine::Renderer
{
/// One command slot in the shared drain: an indirect mesh run or a producer draw.
struct TransparentCompositionSlot
{
    uint32 Index = 0;
    bool Contributor = false;
};

/// Orders all artwork together, merging only adjacent mesh draws with identical material state.
/// Producer draws split mesh runs even when the meshes on both sides use the same material.
void BuildTransparentComposition(
    std::span<const SortedTransparentRecord> records, std::span<const RenderSortKey> keys,
    std::span<const ECS::EntityHandle> entities, std::span<const SortedTransparentCommand> contributors,
    std::pmr::memory_resource& scratch, SortedTransparentRunPartition& partition,
    std::vector<uint32>& meshOrder, std::vector<TransparentCompositionSlot>& slots,
    bool directDraws);
} // namespace GameEngine::Engine::Renderer
