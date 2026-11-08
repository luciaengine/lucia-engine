#include "Prefabs/PrefabInstanceDescriptions.h"

#include "Prefabs/PrefabHierarchyRows.h"

#include "ECS/ComponentFieldRegistry.h"
#include "ECS/Entity.h"
#include "Types/StringUtils.h"

#include <algorithm>
#include <utility>

namespace GameEngine::Editor
{

const Scene::PrefabInstanceEntity* DescribedPrefabInstance::Find(ECS::EntityHandle entity) const
{
    const uint32* index = m_IndexOf.Find(entity.id);
    if (!index || Entities[*index].Entity != entity)
        return nullptr;
    return &Entities[*index];
}

PrefabInstanceDescriptions& PrefabInstanceDescriptions::Get()
{
    static PrefabInstanceDescriptions s_Descriptions;
    return s_Descriptions;
}

void PrefabInstanceDescriptions::Track(EditorChangeNotifications& notifications)
{
    const uint64 channel = notifications.GetChannelId();
    if (std::find(m_TrackedChannels.begin(), m_TrackedChannels.end(), channel) != m_TrackedChannels.end())
        return;
    m_TrackedChannels.push_back(channel);
    notifications.SubscribeComponentChanged(
        [this](const EditorChangeNotifications::ComponentChangedEvent& change) { OnComponentChanged(change); });
    notifications.SubscribeWorldStructureChanged(
        [this](const EditorChangeNotifications::WorldStructureChangedEvent& change) { OnWorldStructureChanged(change); });
}

const DescribedPrefabInstance* PrefabInstanceDescriptions::FindInstance(ECS::World& world, ECS::EntityHandle entity)
{
    ResetForWorld(world);
    const ECS::EntityHandle root = FindOutermostInstanceRoot(world, entity);
    if (!root.IsValid())
        return nullptr;
    Entry* entry = m_Instances.Find(root.id);
    if (!entry || entry->Root != root)
    {
        entry = &m_Instances.InsertOrAssign(root.id, Entry{root, nullptr});
        if (Scene::DescribePrefabInstance(world, root, m_Scratch))
        {
            auto description = std::make_unique<DescribedPrefabInstance>();
            description->Root = root;
            description->Entities = std::move(m_Scratch);
            description->m_IndexOf.Reserve(description->Entities.size());
            for (uint32 index = 0; index < description->Entities.size(); ++index)
                description->m_IndexOf.InsertOrAssign(description->Entities[index].Entity.id, index);
            entry->Description = std::move(description);
        }
        m_Scratch.clear();
    }
    return entry->Description.get();
}

const Scene::PrefabInstanceEntity* PrefabInstanceDescriptions::FindEntity(ECS::World& world, ECS::EntityHandle entity)
{
    const DescribedPrefabInstance* instance = FindInstance(world, entity);
    return instance ? instance->Find(entity) : nullptr;
}

void PrefabInstanceDescriptions::Forget(ECS::World& world, ECS::EntityHandle entity)
{
    ResetForWorld(world);
    const ECS::EntityHandle root = FindOutermostInstanceRoot(world, entity);
    if (root.IsValid())
        m_Instances.Erase(root.id);
}

void PrefabInstanceDescriptions::ResetForWorld(const ECS::World& world)
{
    if (m_WorldId == world.GetWorldId() && m_Epoch == world.GetEntityHandleEpoch())
        return;
    m_WorldId = world.GetWorldId();
    m_Epoch = world.GetEntityHandleEpoch();
    m_Instances.Clear();
}

void PrefabInstanceDescriptions::OnComponentChanged(const EditorChangeNotifications::ComponentChangedEvent& change)
{
    // A drag reports each step; the description changes with the commit.
    if (change.kind == EditorChangeNotifications::ChangeKind::Preview || !change.world ||
        change.world->GetWorldId() != m_WorldId)
        return;
    const ECS::EntityHandle root = FindOutermostInstanceRoot(*change.world, change.entity);
    if (root.IsValid())
        m_Instances.Erase(root.id);
}

void PrefabInstanceDescriptions::OnWorldStructureChanged(
    const EditorChangeNotifications::WorldStructureChangedEvent& change)
{
    // An entity moved into, out of or between instances.
    if (change.world && change.world->GetWorldId() == m_WorldId)
        m_Instances.Clear();
}

std::string_view ReflectedFieldForSceneKey(ECS::ComponentTypeId typeId, std::string_view key)
{
    std::string_view arrayField;
    for (const ECS::FieldInfo& field : ECS::ComponentFieldRegistry::Get(typeId))
    {
        if (EqualsIgnoreCase(key, field.Name))
            return field.Name;
        const bool indexFollows = key.size() > field.Name.size() && StartsWithIgnoreCase(key, field.Name) &&
                                  key[field.Name.size()] >= '0' && key[field.Name.size()] <= '9';
        if (indexFollows && field.Name.size() > arrayField.size())
            arrayField = field.Name;
    }
    return arrayField;
}

void NotifyPrefabIdentityChanges(EditorChangeNotifications* notifications, ECS::World& world,
                                 std::span<const Scene::PrefabIdentityBefore> members)
{
    NotifyWorldStructure(notifications, &world);
    if (!notifications)
        return;
    for (const Scene::PrefabIdentityBefore& member : members)
        if (world.IsValid(member.Entity))
            notifications->NotifyComponentChanged(
                {&world, member.Entity, 0, EditorChangeNotifications::ChangeKind::InspectorRebuild});
}

} // namespace GameEngine::Editor
