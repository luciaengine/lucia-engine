#pragma once

#include "Scripting/ScriptingABI.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Events summarize one update: FrameChanged=1, Looped=2, Finished=4.
typedef struct GE_SpriteAnimationEvents
{
    uint32_t flags;
    uint32_t frameIndex;
    int64_t loopCount;
    float timeSeconds;
} GE_SpriteAnimationEvents;

/// Main thread only. Consumes the latest update's events once, until playback updates again.
/// A zero flags value has no payload. Pausing does not produce Finished.
GE_API GE_Result GE_CDECL GE_SpriteAnimation_PollEvents(GE_Handle worldHandle, uint64_t entityId,
                                                       GE_SpriteAnimationEvents* outEvents);

#ifdef __cplusplus
}
#endif
