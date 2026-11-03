#pragma once

#include "Types/Types.h"

namespace GameEngine::Components
{
/// Authored painter order for transparent artwork. Opaque geometry retains depth ordering.
struct RenderSorting
{
    int32 Layer = 0;
    int32 OrderInLayer = 0;
    /// The outermost enabled group sorts its descendants as one contiguous unit.
    bool GroupChildren = false;
};
}
