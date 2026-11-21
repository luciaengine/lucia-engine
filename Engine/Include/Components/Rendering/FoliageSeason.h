#pragma once

#include "Mathematics/Vector3.h"
#include "Types/Types.h"

namespace GameEngine::Components
{

// World-wide seasonal shading. The highest priority enabled component supplies
// the state. Materials control their own autumn and snow receptivity.
struct FoliageSeason
{
    Mathematics::Vector3 AutumnTint{1.35f, 0.55f, 0.20f};
    float AutumnAmount = 0.0f;
    float SnowAmount = 0.0f;
    int32 Priority = 0;
};

} // namespace GameEngine::Components
