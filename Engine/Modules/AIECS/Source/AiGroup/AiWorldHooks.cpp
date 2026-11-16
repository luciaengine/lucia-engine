#include "AIECS/AiGroupKnowledge.h"

#include "AIECS/Components/AiGroup.h"
#include "AiGroup/AiGroupStore.h"
#include "ECS/Entity.h"

namespace GameEngine::AIECS
{

namespace
{

// A hook runs under the lock of the world, so it reads only the world id and the component.
void ReleaseGroupKnowledge(const ECS::World& world, ECS::EntityHandle entity, Components::AiGroup& group)
{
    AiGroupStore::ReleaseInWorld(world.GetWorldId(), entity, group.Knowledge);
}

} // namespace

void RegisterAiWorldHooks(ECS::World& world)
{
    world.RegisterOnRemove<Components::AiGroup>(&ReleaseGroupKnowledge);
}

} // namespace GameEngine::AIECS
