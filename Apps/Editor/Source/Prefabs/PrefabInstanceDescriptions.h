#pragma once

#include "EditorChangeNotifications.h"

#include "ECS/ECS.h"
#include "Scene/PrefabAuthoring.h"
#include "Types/FlatMap.h"

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine
{
}

namespace GameEngine
{
struct OverrideMenuEntry;
struct OverrideMenuTarget;
}

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::Editor
{
class UndoRedoService;

/// One outermost prefab instance as Scene::DescribePrefabInstance reports it.
struct DescribedPrefabInstance
{
    ECS::EntityHandle Root;
    /// The root, then the entities under it, parents first.
    std::vector<Scene::PrefabInstanceEntity> Entities;

    /// The entry of `entity`; null for an entity that is not part of the instance.
    const Scene::PrefabInstanceEntity* Find(ECS::EntityHandle entity) const;

  private:
    friend class PrefabInstanceDescriptions;
    FlatMap<uint32, uint32> m_IndexOf; // entity id -> its index in Entities
};

/// The descriptions of the prefab instances of one world, which the Hierarchy rows, the Inspector's
/// prefab bar and its override marks read. An instance keeps its description until the editor reports
/// a committed change to one of its entities or a change to the world's structure. Main thread only.
class PrefabInstanceDescriptions final
{
  public:
    static PrefabInstanceDescriptions& Get();

    /// Drops the descriptions that the changes `notifications` reports make stale. A later call with a
    /// channel tracked before does nothing. The channel owns the subscriptions, so a destroyed channel takes
    /// them with it.
    void Track(EditorChangeNotifications& notifications);

    /// The description of the outermost instance that holds `entity`. Null when `entity` is in no
    /// instance or the instance's prefab is not resident. Valid until the next call on this object.
    const DescribedPrefabInstance* FindInstance(ECS::World& world, ECS::EntityHandle entity);

    /// The description of `entity` in its outermost instance; null outside every described instance.
    /// Valid until the next call on this object.
    const Scene::PrefabInstanceEntity* FindEntity(ECS::World& world, ECS::EntityHandle entity);

    /// Drops the description of the instance that holds `entity`, so the next read describes it again.
    void Forget(ECS::World& world, ECS::EntityHandle entity);

  private:
    void ResetForWorld(const ECS::World& world);
    void OnComponentChanged(const EditorChangeNotifications::ComponentChangedEvent& change);
    void OnWorldStructureChanged(const EditorChangeNotifications::WorldStructureChangedEvent& change);

    uint64 m_WorldId = 0;
    uint64 m_Epoch = 0;
    struct Entry
    {
        ECS::EntityHandle Root;
        // Null for an instance whose prefab is not resident.
        std::unique_ptr<DescribedPrefabInstance> Description;
    };

    FlatMap<uint32, Entry> m_Instances; // root id -> its entry
    std::vector<Scene::PrefabInstanceEntity> m_Scratch;
    // The GetChannelId of each tracked channel. An id is never given to a second channel.
    std::vector<uint64> m_TrackedChannels;
};

/// Refreshes the Inspector banners of entities whose prefab identity changed.
void NotifyPrefabIdentityChanges(EditorChangeNotifications* notifications, ECS::World& world,
                                 std::span<const Scene::PrefabIdentityBefore> members);

/// The reflected field of `typeId` that the scene key `key` of an override writes: the field of that name, or
/// the array field whose name the element's index follows. Empty when no reflected field matches.
std::string_view ReflectedFieldForSceneKey(ECS::ComponentTypeId typeId, std::string_view key);

/// Apply and Revert menu entries for an instance field or component header.
std::vector<OverrideMenuEntry> QueryPrefabOverrideMenu(const OverrideMenuTarget& target);

/// Writes `targets` into their prefab files as one undo step named `undoName`. A refusal shows a notice.
void ApplyPrefabOverridesAsOneStep(ECS::World& world, std::span<const Scene::PrefabOverrideTarget> targets,
                                   UndoRedoService* undo, EditorChangeNotifications* notifications,
                                   const std::string& undoName);

/// Gives each entity of `targets` the prefab's value of its override, as one undo step named `undoName`. A refusal
/// shows a notice.
void RevertPrefabOverridesAsOneStep(ECS::World& world, std::span<const Scene::PrefabOverrideTarget> targets,
                                    UndoRedoService* undo, EditorChangeNotifications* notifications,
                                    const std::string& undoName);

} // namespace GameEngine::Editor
