#pragma once

#include "Types/Types.h"

namespace GameEngine::Components
{
// A rectangular mirror on the entity's local XZ plane, facing local +Y.
// Translation/rotation place the plane; X/Z scale set its width/height.
// Add to a flat mesh, or place an independent plane over a coplanar surface.
// This is a per-camera scene capture, independent of ReflectionProbe.
struct PlanarReflection
{
    float32 ResolutionScale = 0.5f; // @ge-range 0.125 1
    float32 MaxRoughness = 0.35f; // @ge-range 0 1
    uint32 Priority = 0;
    uint32 CullMask = 0xFFFFFFFFu;
};
} // namespace GameEngine::Components
