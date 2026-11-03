#pragma once

#include "Engine/Rendering/DrawCommand.h"
#include "Engine/Rendering/RenderSortKey.h"

namespace GameEngine::Engine::Renderer
{
/// A producer draw participating in the same ordered stream as transparent meshes.
/// Binding spans refer to producer-owned storage that remains alive until frame execution.
struct SortedTransparentCommand
{
    DrawCommand Command;
    RenderSortKey Sorting;
    // Stable producer family breaks otherwise equal keys before entity identity,
    // keeping same-key instance batches contiguous. Mesh submissions use zero.
    StringId Family = 0;
    ECS::EntityHandle Entity;
    uint32 SubOrder = 0;
};
}
