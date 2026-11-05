#include "Scripting/SpriteAnimationABI.h"

#include "AbiHandles.h"
#include "AbiMainThread.h"
#include "Components/Animation/AnimatedSprite2D.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"

#include <cstddef>

static_assert(sizeof(GE_SpriteAnimationEvents) == 24);
static_assert(offsetof(GE_SpriteAnimationEvents, flags) == 0);
static_assert(offsetof(GE_SpriteAnimationEvents, frameIndex) == 4);
static_assert(offsetof(GE_SpriteAnimationEvents, loopCount) == 8);
static_assert(offsetof(GE_SpriteAnimationEvents, timeSeconds) == 16);

GE_API GE_Result GE_CDECL GE_SpriteAnimation_PollEvents(GE_Handle worldHandle, uint64_t entityId,
                                                       GE_SpriteAnimationEvents* outEvents)
{
    if (!outEvents)
        return GE_Result_InvalidArg;
    *outEvents = {};
    if (!worldHandle || entityId > UINT32_MAX)
        return GE_Result_InvalidArg;
    if (!GameEngine::ScriptingAbi::OnEngineMainThread())
        return GE_Result_Fail;
    try
    {
        auto* world = GameEngine::ScriptingAbi::WorldFromHandle(worldHandle);
        const auto entity = GameEngine::ScriptingAbi::EntityFromId(entityId);
        if (!world->IsValid(entity))
            return GE_Result_NotFound;
        const auto* sprite = world->GetComponent<GameEngine::Components::AnimatedSprite2D>(entity);
        if (!sprite)
            return GE_Result_NotFound;
        if (sprite->PendingPlaybackEvents == 0)
            return GE_Result_Ok;
        *outEvents = {sprite->PendingPlaybackEvents, sprite->PlaybackEventFrame,
                      sprite->PlaybackEventLoops, sprite->TimeSeconds};
        world->GetComponentForWrite<GameEngine::Components::AnimatedSprite2D>(entity)->PendingPlaybackEvents = 0;
        return GE_Result_Ok;
    }
    catch (...)
    {
        *outEvents = {};
        return GE_Result_Fail;
    }
}
