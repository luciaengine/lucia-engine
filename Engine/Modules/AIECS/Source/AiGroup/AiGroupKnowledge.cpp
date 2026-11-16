#include "AIECS/AiGroupKnowledge.h"

#include "AIECS/Components/AiGroup.h"
#include "AiGroup/AiGroupStore.h"
#include "ECS/Entity.h"

namespace GameEngine::AIECS
{

namespace
{

AiGroupStore::Group* FindGroup(const ECS::World& world, ECS::EntityHandle entity)
{
    const Components::AiGroup* group = world.GetComponent<Components::AiGroup>(entity);
    AiGroupStore* store = group != nullptr ? AiGroupStore::FindForWorld(world.GetWorldId()) : nullptr;
    return store != nullptr ? store->Find(entity, group->Knowledge) : nullptr;
}

} // namespace

const AI::RecordStore* FindGroupRecords(const ECS::World& world, ECS::EntityHandle group)
{
    const AiGroupStore::Group* found = FindGroup(world, group);
    return found != nullptr ? &found->Records : nullptr;
}

std::span<uint32> FindGroupPlan(ECS::World& world, ECS::EntityHandle group)
{
    AiGroupStore::Group* found = FindGroup(world, group);
    return found != nullptr ? std::span<uint32>(found->Plan) : std::span<uint32>();
}

} // namespace GameEngine::AIECS
