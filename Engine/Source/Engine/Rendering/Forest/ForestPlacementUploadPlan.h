#pragma once

#include "ForestPlacementStream.h"

#include <optional>
#include <span>
#include <vector>

namespace GameEngine::Engine::Renderer
{

/// The bytes of one placement slot in the slot buffer.
inline constexpr uint64 kPlacementSlotBytes =
    PageStreaming::PackedPlacementStore::kPlacementsPerPage * sizeof(PageStreaming::PackedPlacement);

/// One copy from the staging bytes into the slot buffer.
struct ForestPlacementCopyRun
{
    uint64 StagingOffset;
    uint64 SlotOffset;
    uint64 Bytes;
};

/// The uploads of one frame in slot order, with byte-adjacent slots merged into one copy.
struct ForestPlacementUploadPlan
{
    std::vector<const ForestPlacementUpload*> SlotOrder;
    std::vector<ForestPlacementCopyRun> Runs;
    uint64 Bytes = 0;
};

/// Plans exact byte copies in slot order. The plan borrows the upload records until the caller packs
/// the staging bytes. Returns nothing for a slot outside the buffer, two uploads into one slot or a
/// page larger than a slot.
std::optional<ForestPlacementUploadPlan> BuildForestPlacementUploadPlan(std::span<const ForestPlacementUpload> uploads,
                                                                        uint32 slotCount);

} // namespace GameEngine::Engine::Renderer
