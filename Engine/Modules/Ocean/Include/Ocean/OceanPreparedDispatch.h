#pragma once

#include "Ocean/OceanCascadeArray.h"
#include "Rendering/Core/Handle.h"
#include "Rendering/Core/RenderGraph/RGFrameStamp.h"

#include <cstdint>

namespace GameEngine::Rendering
{
class CommandList;
}

namespace GameEngine::Ocean
{
// One graph pass owns each packet, including when several simulation steps are live.
struct OceanPreparedDispatch
{
    Rendering::PipelineHandle Pipeline{};
    Rendering::DescriptorSetHandle Set{};
    uint32_t GroupsX = 0;
    uint32_t GroupsY = 0;
    uint32_t GroupsZ = 0;
    Rendering::TextureHandle Target{};

    bool IsValid() const
    {
        return Pipeline.IsValid() && Set.IsValid() && GroupsX != 0 && GroupsY != 0 && GroupsZ != 0;
    }
};

struct OceanPreparedSimulationDispatch
{
    OceanPreparedDispatch Commands;
    OceanCascadeStep Step;
    Rendering::RenderGraph::RGFrameStamp Owner;
    const OceanCascadeFramePlan* Plan = nullptr;
    uint32_t SubstepIndex = 0;

    bool IsValid() const { return Commands.IsValid() && Plan != nullptr; }
};

struct OceanPreparedReadbackDispatch
{
    OceanPreparedDispatch Commands;
    Rendering::RenderGraph::RGFrameStamp Owner;
    Rendering::BufferHandle Output;
    uint64_t Reservation = 0;
};

bool RecordOceanPreparedDispatch(Rendering::CommandList* commands, const OceanPreparedDispatch& dispatch);
} // namespace GameEngine::Ocean
